import type { NotificationIntent, ScheduleReceiptIntent } from '../../contracts/device-gateway.js';
import { parseNotificationIntent, parseScheduleReceiptIntent } from '../../contracts/device-gateway-parser.js';
import type { ChannelAccountId } from '../../contracts/ids.js';
import type { ChannelAccount, ChannelCapabilities, Delivery } from '../../domain/models.js';
import type { ImSendAcceptance, OutboundImMessage } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { JsonValue } from '../../shared/types.js';

const MAX_API_RESPONSE_BYTES = 64 * 1024;
const ACCESS_TOKEN_REFRESH_MARGIN_SECONDS = 60;
const WECHAT_TOKEN_ERRORS = new Set([40014, 42001]);
const WECHAT_RETRYABLE_ERRORS = new Set([-1, 42001, 45009, 45011]);
const TEXT_ENCODER = new TextEncoder();

/** 微信模板消息字段名配置。 */
export interface WechatTemplateFields {
    readonly title: string;
    readonly body: string;
    readonly time: string;
}

/** 由部署层从 Secret 引用解析后注入的微信公众号出站配置。 */
export interface WechatOfficialOutboundOptions {
    readonly appId: string;
    readonly appSecret: string;
    readonly templateId: string;
    readonly templateFields: WechatTemplateFields;
    readonly actionUiBaseUrl: string;
    /** 将持久化的受保护外部身份还原为微信 openid；实现不得记录明文。 */
    readonly revealExternalUserId: (ciphertext: string) => Promise<string>;
    /** 可替换的 Fetch 实现，仅测试或受控运行时注入。 */
    readonly fetch?: typeof fetch;
}

interface NormalizedWechatOutboundOptions extends Omit<WechatOfficialOutboundOptions, 'fetch'> {
    readonly fetch: typeof fetch;
}

interface WechatTemplatePayload {
    readonly [key: string]: JsonValue;
    readonly type: 'wechat_template';
    readonly templateId: string;
    readonly data: Readonly<Record<string, { readonly [key: string]: JsonValue; readonly value: string }>>;
    readonly url?: string;
}

/** 封装微信公众号 access_token、模板渲染与发送结果分类。 */
export class WechatOfficialOutbound {
    private readonly options: NormalizedWechatOutboundOptions;

    private accessTokenCache: { readonly value: string; readonly expiresAt: number } | undefined;

    /**
     * @param options 部署层从 Secret 引用解析的出站配置。
     * @param now 返回当前 Unix 秒的时钟。
     */
    public constructor(
        options: WechatOfficialOutboundOptions,
        private readonly now: () => number,
    ) {
        this.options = normalizeOptions(options);
    }

    /** @returns 微信模板消息与 H5 降级能力。 */
    public capabilities(): ChannelCapabilities {
        return {
            proactiveMessage: true,
            nativeAction: false,
            actionUi: true,
            deliveryReceipt: true,
            presentationTypes: ['template'],
        };
    }

    /**
     * 渲染日程操作回执模板。
     * @param intent 日程操作回执意图。
     * @returns 微信模板消息载荷。
     */
    public renderScheduleReceipt(intent: ScheduleReceiptIntent): JsonValue {
        return this.templatePayload({ title: '日程已更新', body: intent.summary, time: intent.occurredAt });
    }

    /**
     * 渲染提醒模板与可选 H5 动作地址。
     * @param intent 提醒通知意图。
     * @param actionToken 服务端签发的可选动作令牌。
     * @returns 微信模板消息载荷。
     */
    public renderNotification(intent: NotificationIntent, actionToken?: string): JsonValue {
        if (intent.reminderType === 'strong' && actionToken === undefined) {
            throw new ImGatewayError('invalid_contract', 'Strong WeChat reminders require an Action UI token');
        }
        return this.templatePayload(
            { title: intent.content.title, body: intent.content.body ?? '', time: intent.triggerAt },
            actionToken,
        );
    }

    /**
     * 校验投递范围并将语义载荷渲染为微信模板。
     * @param channelAccountId 当前 Adapter 所属渠道账号。
     * @param delivery 待渲染投递。
     * @param account 目标渠道账号。
     * @param capabilities 已解析的渠道能力。
     * @param actionToken 服务端签发的可选动作令牌。
     * @returns 微信模板消息载荷。
     */
    public render(
        channelAccountId: ChannelAccountId,
        delivery: Delivery,
        account: ChannelAccount,
        capabilities: ChannelCapabilities,
        actionToken?: string,
    ): JsonValue {
        if (
            delivery.channelAccountId !== channelAccountId ||
            account.id !== channelAccountId ||
            account.platform !== 'wechat_official' ||
            delivery.presentationType !== 'template' ||
            !capabilities.presentationTypes.includes('template')
        ) {
            throw new ImGatewayError('capability_not_supported', 'WeChat delivery target is invalid');
        }
        return delivery.kind === 'schedule_receipt'
            ? this.renderScheduleReceipt(parseScheduleReceiptIntent(delivery.semanticPayload))
            : this.renderNotification(parseNotificationIntent(delivery.semanticPayload), actionToken);
    }

    /**
     * 取得 access_token 并调用微信模板发送接口。
     * @param channelAccountId 当前 Adapter 所属渠道账号。
     * @param message 已渲染的出站消息。
     * @returns 平台即时受理结果，不把受理误记为用户已收到。
     */
    public async send(channelAccountId: ChannelAccountId, message: OutboundImMessage): Promise<ImSendAcceptance> {
        if (message.delivery.channelAccountId !== undefined && message.delivery.channelAccountId !== channelAccountId) {
            return { accepted: false, retryable: false, errorCode: 'wechat_account_mismatch' };
        }
        const payload = parseTemplatePayload(message.content);
        const externalUserId = await this.options.revealExternalUserId(
            message.conversation.externalConversationIdCiphertext,
        );
        if (externalUserId.trim() === '' || externalUserId.length > 128) {
            return { accepted: false, retryable: false, errorCode: 'wechat_invalid_recipient' };
        }
        let result: WechatApiResult;
        try {
            result = await this.sendTemplateRequest(externalUserId, payload, false);
            if (WECHAT_TOKEN_ERRORS.has(result.errcode)) {
                this.accessTokenCache = undefined;
                result = await this.sendTemplateRequest(externalUserId, payload, true);
            }
        } catch (error) {
            if (error instanceof WechatAccessTokenError) {
                return {
                    accepted: false,
                    retryable: WECHAT_RETRYABLE_ERRORS.has(error.errcode),
                    errorCode: `wechat_${String(error.errcode)}`,
                };
            }
            throw error;
        }
        if (result.errcode !== 0) {
            return {
                accepted: false,
                retryable: WECHAT_RETRYABLE_ERRORS.has(result.errcode),
                errorCode: `wechat_${String(result.errcode)}`,
            };
        }
        return result.msgid === undefined
            ? { accepted: false, retryable: true, errorCode: 'wechat_missing_msgid' }
            : { accepted: true, platformMessageId: result.msgid };
    }

    private templatePayload(
        content: { readonly title: string; readonly body: string; readonly time: string },
        actionToken?: string,
    ): WechatTemplatePayload {
        const fields = this.options.templateFields;
        return {
            type: 'wechat_template',
            templateId: this.options.templateId,
            data: {
                [fields.title]: { value: content.title },
                [fields.body]: { value: content.body },
                [fields.time]: { value: content.time },
            },
            ...(actionToken === undefined
                ? {}
                : { url: `${this.options.actionUiBaseUrl}/${encodeURIComponent(actionToken)}` }),
        };
    }

    private async accessToken(forceRefresh: boolean): Promise<string> {
        if (
            !forceRefresh &&
            this.accessTokenCache !== undefined &&
            this.accessTokenCache.expiresAt > this.now() + ACCESS_TOKEN_REFRESH_MARGIN_SECONDS
        ) {
            return this.accessTokenCache.value;
        }
        const url = new URL('https://api.weixin.qq.com/cgi-bin/token');
        url.searchParams.set('grant_type', 'client_credential');
        url.searchParams.set('appid', this.options.appId);
        url.searchParams.set('secret', this.options.appSecret);
        const response = await this.options.fetch(url, { headers: { accept: 'application/json' } });
        const result = await readWechatApiResponse(response);
        if (
            !response.ok ||
            result.errcode !== 0 ||
            result.accessToken === undefined ||
            result.accessToken.trim() === ''
        ) {
            throw new WechatAccessTokenError(result.errcode);
        }
        this.accessTokenCache = {
            value: result.accessToken,
            expiresAt: this.now() + Math.max(1, result.expiresIn ?? 7200),
        };
        return result.accessToken;
    }

    private async sendTemplateRequest(
        externalUserId: string,
        payload: WechatTemplatePayload,
        forceRefresh: boolean,
    ): Promise<WechatApiResult> {
        const url = new URL('https://api.weixin.qq.com/cgi-bin/message/template/send');
        url.searchParams.set('access_token', await this.accessToken(forceRefresh));
        const response = await this.options.fetch(url, {
            method: 'POST',
            headers: { accept: 'application/json', 'content-type': 'application/json; charset=utf-8' },
            body: JSON.stringify({
                touser: externalUserId,
                template_id: payload.templateId,
                data: payload.data,
                ...(payload.url === undefined ? {} : { url: payload.url }),
            }),
        });
        const result = await readWechatApiResponse(response);
        return !response.ok && result.errcode === 0
            ? { errcode: response.status >= 500 ? -1 : response.status }
            : result;
    }
}

interface WechatApiResult {
    readonly errcode: number;
    readonly msgid?: string;
    readonly accessToken?: string;
    readonly expiresIn?: number;
}

class WechatAccessTokenError extends Error {
    public constructor(public readonly errcode: number) {
        super('WeChat access token request was rejected');
    }
}

function normalizeOptions(options: WechatOfficialOutboundOptions): NormalizedWechatOutboundOptions {
    const appId = options.appId.trim();
    const appSecret = options.appSecret.trim();
    const templateId = options.templateId.trim();
    if (!/^wx[A-Za-z0-9_-]{1,126}$/u.test(appId) || appSecret === '' || appSecret.length > 256) {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound credentials are invalid');
    }
    if (templateId === '' || templateId.length > 128 || !/^[A-Za-z0-9_-]+$/u.test(templateId)) {
        throw new ImGatewayError('invalid_contract', 'WeChat template id is invalid');
    }
    const fields = {
        title: templateField(options.templateFields.title),
        body: templateField(options.templateFields.body),
        time: templateField(options.templateFields.time),
    };
    if (new Set(Object.values(fields)).size !== 3) {
        throw new ImGatewayError('invalid_contract', 'WeChat template fields must be distinct');
    }
    let actionUiUrl: URL;
    try {
        actionUiUrl = new URL(options.actionUiBaseUrl);
    } catch {
        throw new ImGatewayError('invalid_contract', 'WeChat Action UI base URL is invalid');
    }
    if (
        actionUiUrl.protocol !== 'https:' ||
        actionUiUrl.username !== '' ||
        actionUiUrl.password !== '' ||
        actionUiUrl.search !== '' ||
        actionUiUrl.hash !== ''
    ) {
        throw new ImGatewayError('invalid_contract', 'WeChat Action UI base URL must be an HTTPS path');
    }
    const fetchImpl = options.fetch ?? globalThis.fetch;
    if (typeof fetchImpl !== 'function') {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound delivery requires Fetch');
    }
    return {
        appId,
        appSecret,
        templateId,
        templateFields: fields,
        actionUiBaseUrl: actionUiUrl.toString().replace(/\/$/u, ''),
        revealExternalUserId: options.revealExternalUserId,
        fetch: fetchImpl,
    };
}

function templateField(value: string): string {
    const field = value.trim();
    if (!/^[A-Za-z][A-Za-z0-9_]{0,63}$/u.test(field)) {
        throw new ImGatewayError('invalid_contract', 'WeChat template field name is invalid');
    }
    return field;
}

function parseTemplatePayload(value: JsonValue): WechatTemplatePayload {
    if (!isRecord(value) || value.type !== 'wechat_template') {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound payload is invalid');
    }
    const templateId = requiredString(value.templateId, 'templateId');
    if (!isRecord(value.data) || Object.keys(value.data).length === 0) {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound template data is invalid');
    }
    const data: Record<string, { readonly value: string }> = {};
    for (const [field, item] of Object.entries(value.data)) {
        if (!/^[A-Za-z][A-Za-z0-9_]{0,63}$/u.test(field) || !isRecord(item) || typeof item.value !== 'string') {
            throw new ImGatewayError('invalid_contract', 'WeChat outbound template data is invalid');
        }
        data[field] = { value: item.value };
    }
    const url = value.url === undefined ? undefined : requiredString(value.url, 'url');
    if (url !== undefined) {
        try {
            if (new URL(url).protocol !== 'https:') throw new Error('not https');
        } catch {
            throw new ImGatewayError('invalid_contract', 'WeChat outbound Action UI URL must use HTTPS');
        }
    }
    return { type: 'wechat_template', templateId, data, ...(url === undefined ? {} : { url }) };
}

function requiredString(value: unknown, field: string): string {
    if (typeof value !== 'string' || value.trim() === '') {
        throw new ImGatewayError('invalid_contract', `WeChat outbound ${field} is invalid`);
    }
    return value;
}

async function readWechatApiResponse(response: Response): Promise<WechatApiResult> {
    const contentLength = response.headers.get('content-length');
    if (contentLength !== null && Number(contentLength) > MAX_API_RESPONSE_BYTES) {
        throw new Error('WeChat API response exceeded the size limit');
    }
    const raw = await response.text();
    if (TEXT_ENCODER.encode(raw).byteLength > MAX_API_RESPONSE_BYTES) {
        throw new Error('WeChat API response exceeded the size limit');
    }
    let parsed: unknown;
    try {
        parsed = JSON.parse(raw);
    } catch {
        throw new Error('WeChat API returned invalid JSON');
    }
    if (!isRecord(parsed)) throw new Error('WeChat API returned an invalid object');
    const errcode = parsed.errcode === undefined ? 0 : parsed.errcode;
    if (typeof errcode !== 'number' || !Number.isSafeInteger(errcode)) {
        throw new Error('WeChat API returned an invalid error code');
    }
    const exactMessageId = /"msgid"\s*:\s*(?:"([0-9]{1,64})"|([0-9]{1,64}))/u.exec(raw);
    const msgid = exactMessageId?.[1] ?? exactMessageId?.[2];
    const accessToken = typeof parsed.access_token === 'string' ? parsed.access_token : undefined;
    const expiresIn =
        typeof parsed.expires_in === 'number' && Number.isSafeInteger(parsed.expires_in) && parsed.expires_in > 0
            ? parsed.expires_in
            : undefined;
    return {
        errcode,
        ...(msgid === undefined ? {} : { msgid }),
        ...(accessToken === undefined ? {} : { accessToken }),
        ...(expiresIn === undefined ? {} : { expiresIn }),
    };
}

function isRecord(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}
