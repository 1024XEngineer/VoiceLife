import type {
    NotificationIntent,
    ScheduleReceiptIntent,
    ScheduleQueryResultIntent,
} from '../../contracts/device-gateway.js';
import {
    parseNotificationIntent,
    parseScheduleReceiptIntent,
    parseScheduleQueryResultIntent,
} from '../../contracts/device-gateway-parser.js';
import type { ChannelAccountId } from '../../contracts/ids.js';
import type { ChannelAccount, ChannelCapabilities, Delivery } from '../../domain/models.js';
import type { ImSendAcceptance, OutboundImMessage } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { JsonValue } from '../../shared/types.js';
import { fetchWithTimeout, parseTemplatePayload, readWechatApiResponse } from './wechat-official-outbound-support.js';

const ACCESS_TOKEN_REFRESH_MARGIN_SECONDS = 60;
const WECHAT_TOKEN_ERRORS = new Set([40014, 42001]);
const WECHAT_RETRYABLE_ERRORS = new Set([-1, 408, 429, 42001, 45009, 45011, 502, 503, 504]);
const DEFAULT_REQUEST_TIMEOUT_MS = 10_000;
const MAX_REQUEST_TIMEOUT_MS = 10_000;

/** 微信模板消息字段名配置。 */
export interface WechatTemplateFields {
    readonly title: string;
    readonly body: string;
    readonly time: string;
}

interface WechatTemplateConfig {
    readonly templateId: string;
    readonly templateFields: WechatTemplateFields;
}

/** 由部署层从 Secret 引用解析后注入的微信公众号出站配置。 */
export interface WechatOfficialOutboundOptions {
    readonly appId: string;
    readonly appSecret: string;
    readonly templateId: string;
    readonly templateFields: WechatTemplateFields;
    /** 查询结果使用的独立模板，提醒和日程回执仍使用上面的默认模板。 */
    readonly queryTemplateId: string;
    readonly queryTemplateFields: WechatTemplateFields;
    /** 模板中展示时间使用的 IANA 时区；默认 Asia/Shanghai。 */
    readonly displayTimeZone?: string;
    readonly actionUiBaseUrl: string;
    /** 将持久化的受保护外部身份还原为微信 openid；实现不得记录明文。 */
    readonly revealExternalUserId: (ciphertext: string) => Promise<string>;
    /** 可替换的 Fetch 实现，仅测试或受控运行时注入。 */
    readonly fetch?: typeof fetch;
    /** 微信 HTTP 请求的截止时间，默认 10 秒。 */
    readonly requestTimeoutMs?: number;
}

interface NormalizedWechatOutboundOptions extends Omit<WechatOfficialOutboundOptions, 'fetch' | 'displayTimeZone'> {
    readonly displayTimeZone: string;
    readonly fetch: typeof fetch;
    readonly requestTimeoutMs: number;
}

type WechatApiResult = Awaited<ReturnType<typeof readWechatApiResponse>>;
type WechatTemplatePayload = ReturnType<typeof parseTemplatePayload>;

/** 封装微信公众号 access_token、模板渲染与发送结果分类。 */
export class WechatOfficialOutbound {
    private readonly options: NormalizedWechatOutboundOptions;

    private accessTokenCache: { readonly value: string; readonly expiresAt: number } | undefined;

    private accessTokenRefresh: Promise<string> | undefined;

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
        return this.templatePayload(
            {
                title: '日程已更新',
                body: intent.summary,
                time: formatTemplateTime(intent.occurredAt, this.options.displayTimeZone),
            },
            this.options,
        );
    }

    /**
     * 将查询范围、结果条目和例外完整保留在 IM 消息正文中。
     * @param intent 完整日程查询结果意图。
     * @returns 微信模板消息载荷。
     */
    public renderScheduleQueryResult(intent: ScheduleQueryResultIntent): JsonValue {
        const body = formatScheduleQueryBody(intent, this.options.displayTimeZone);
        return this.templatePayload(
            {
                title: `日程查询结果（${intent.resultCount} 条）`,
                body,
                time: formatTemplateTime(intent.queriedAt, this.options.displayTimeZone),
            },
            {
                templateId: this.options.queryTemplateId,
                templateFields: this.options.queryTemplateFields,
            },
        );
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
            {
                title: intent.content.title,
                body: intent.content.body ?? '',
                time: formatTemplateTime(intent.triggerAt, this.options.displayTimeZone),
            },
            this.options,
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
        if (delivery.kind === 'schedule_receipt') {
            return this.renderScheduleReceipt(parseScheduleReceiptIntent(delivery.semanticPayload));
        }
        if (delivery.kind === 'schedule_query_result') {
            return this.renderScheduleQueryResult(parseScheduleQueryResultIntent(delivery.semanticPayload));
        }
        return this.renderNotification(parseNotificationIntent(delivery.semanticPayload), actionToken);
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
        const revealedExternalUserId = await this.options.revealExternalUserId(
            message.conversation.externalConversationIdCiphertext,
        );
        return this.sendToUser(revealedExternalUserId, message.content);
    }

    /**
     * 发送已由 Koishi Channel Adapter 解密目标并渲染的模板消息。
     * @param revealedExternalUserId 当前调用使用的微信 OpenID。
     * @param content 已渲染的微信模板载荷。
     * @returns 平台即时受理结果。
     */
    public async sendToUser(revealedExternalUserId: string, content: JsonValue): Promise<ImSendAcceptance> {
        const template = templateConfigForContent(content, this.options);
        const payload = parseTemplatePayload(
            content,
            template.templateId,
            template.templateFields,
            this.options.actionUiBaseUrl,
        );
        const externalUserId = revealedExternalUserId.trim();
        if (!/^[A-Za-z0-9_-]{1,128}$/u.test(externalUserId)) {
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
            if (isNamedError(error, 'WechatProtocolError')) {
                return { accepted: false, retryable: false, errorCode: 'wechat_protocol_error' };
            }
            if (isAbortError(error)) {
                return { accepted: false, retryable: true, errorCode: 'wechat_timeout' };
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
        template: WechatTemplateConfig,
        actionToken?: string,
    ): WechatTemplatePayload {
        const fields = template.templateFields;
        return {
            type: 'wechat_template',
            templateId: template.templateId,
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
        if (this.accessTokenRefresh !== undefined) return this.accessTokenRefresh;
        const refresh = this.fetchAccessToken();
        this.accessTokenRefresh = refresh;
        try {
            return await refresh;
        } finally {
            if (this.accessTokenRefresh === refresh) this.accessTokenRefresh = undefined;
        }
    }

    private async fetchAccessToken(): Promise<string> {
        const url = new URL('https://api.weixin.qq.com/cgi-bin/token');
        url.searchParams.set('grant_type', 'client_credential');
        url.searchParams.set('appid', this.options.appId);
        url.searchParams.set('secret', this.options.appSecret);
        const response = await fetchWithTimeout(
            this.options.fetch,
            url,
            { headers: { accept: 'application/json' } },
            this.options.requestTimeoutMs,
        );
        const result = await readWechatApiResponse(response);
        // 与发送端点一致：非 2xx 且未携带有效微信 errcode 时按 HTTP 状态归类（5xx→-1 重试，429/408→对应码重试，其余→HTTP 状态码永久）。
        if (!response.ok && result.errcode === 0) {
            throw new WechatAccessTokenError(response.status >= 500 ? -1 : response.status);
        }
        if (result.errcode !== 0 || result.accessToken === undefined || result.accessToken.trim() === '') {
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
        const response = await fetchWithTimeout(
            this.options.fetch,
            url,
            {
                method: 'POST',
                headers: { accept: 'application/json', 'content-type': 'application/json; charset=utf-8' },
                body: JSON.stringify({
                    touser: externalUserId,
                    template_id: payload.templateId,
                    data: payload.data,
                    ...(payload.url === undefined ? {} : { url: payload.url }),
                }),
            },
            this.options.requestTimeoutMs,
        );
        const result = await readWechatApiResponse(response);
        return !response.ok && result.errcode === 0
            ? { errcode: response.status >= 500 ? -1 : response.status }
            : result;
    }
}

class WechatAccessTokenError extends Error {
    public constructor(public readonly errcode: number) {
        super('WeChat access token request was rejected');
    }
}

function templateConfigForContent(content: JsonValue, options: NormalizedWechatOutboundOptions): WechatTemplateConfig {
    if (
        typeof content === 'object' &&
        content !== null &&
        !Array.isArray(content) &&
        'templateId' in content &&
        typeof content.templateId === 'string' &&
        content.templateId === options.queryTemplateId
    ) {
        return {
            templateId: options.queryTemplateId,
            templateFields: options.queryTemplateFields,
        };
    }
    return options;
}

function normalizeOptions(options: WechatOfficialOutboundOptions): NormalizedWechatOutboundOptions {
    const appId = options.appId.trim();
    const appSecret = options.appSecret.trim();
    const templateId = options.templateId.trim();
    const queryTemplateId = options.queryTemplateId.trim();
    if (!/^wx[A-Za-z0-9_-]{1,126}$/u.test(appId) || appSecret === '' || appSecret.length > 256) {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound credentials are invalid');
    }
    if (templateId === '' || templateId.length > 128 || !/^[A-Za-z0-9_-]+$/u.test(templateId)) {
        throw new ImGatewayError('invalid_contract', 'WeChat template id is invalid');
    }
    if (queryTemplateId === '' || queryTemplateId.length > 128 || !/^[A-Za-z0-9_-]+$/u.test(queryTemplateId)) {
        throw new ImGatewayError('invalid_contract', 'WeChat query template id is invalid');
    }
    if (templateId === queryTemplateId) {
        throw new ImGatewayError('invalid_contract', 'WeChat template ids must be distinct');
    }
    const fields = {
        title: templateField(options.templateFields.title),
        body: templateField(options.templateFields.body),
        time: templateField(options.templateFields.time),
    };
    if (new Set(Object.values(fields)).size !== 3) {
        throw new ImGatewayError('invalid_contract', 'WeChat template fields must be distinct');
    }
    const queryFields = {
        title: templateField(options.queryTemplateFields.title),
        body: templateField(options.queryTemplateFields.body),
        time: templateField(options.queryTemplateFields.time),
    };
    if (new Set(Object.values(queryFields)).size !== 3) {
        throw new ImGatewayError('invalid_contract', 'WeChat query template fields must be distinct');
    }
    const displayTimeZone = normalizeDisplayTimeZone(options.displayTimeZone);
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
    const requestTimeoutMs = options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
    if (!Number.isInteger(requestTimeoutMs) || requestTimeoutMs < 1 || requestTimeoutMs > MAX_REQUEST_TIMEOUT_MS) {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound request timeout is invalid');
    }
    return {
        appId,
        appSecret,
        templateId,
        templateFields: fields,
        queryTemplateId,
        queryTemplateFields: queryFields,
        displayTimeZone,
        actionUiBaseUrl: actionUiUrl.toString().replace(/\/$/u, ''),
        revealExternalUserId: options.revealExternalUserId,
        fetch: fetchImpl,
        requestTimeoutMs,
    };
}

function normalizeDisplayTimeZone(value: string | undefined): string {
    const timeZone = value?.trim() || 'Asia/Shanghai';
    try {
        new Intl.DateTimeFormat('en-US', { timeZone }).format(0);
    } catch {
        throw new ImGatewayError('invalid_contract', 'WeChat display time zone is invalid');
    }
    return timeZone;
}

function formatTemplateTime(value: string, timeZone: string): string {
    const parts = new Intl.DateTimeFormat('zh-CN', {
        timeZone,
        year: 'numeric',
        month: '2-digit',
        day: '2-digit',
        hour: '2-digit',
        minute: '2-digit',
        hour12: false,
    }).formatToParts(new Date(value));
    const part = (type: Intl.DateTimeFormatPartTypes): string => parts.find((item) => item.type === type)?.value ?? '';
    const datePart = (type: 'year' | 'month' | 'day'): string => String(Number(part(type)));
    return `${datePart('year')}年${datePart('month')}月${datePart('day')}日 ${part('hour')}:${part('minute')}`;
}

const QUERY_STATUS_LABELS: Readonly<Record<ScheduleQueryResultIntent['query']['status'], string>> = {
    all: '全部',
    active: '进行中',
    cancelled: '已取消',
    completed: '已完成',
};

const MAX_SCHEDULE_QUERY_BODY_LENGTH = 1000;

/** 将完整的日程查询结果压缩为模板字段可直接阅读的文本。 */
function formatScheduleQueryBody(intent: ScheduleQueryResultIntent, timeZone: string): string {
    const lines = [`共 ${intent.resultCount} 条日程`];
    const entries = [...intent.schedules, ...intent.futureOccurrences];
    if (entries.length === 0) {
        lines.push('暂无符合条件的日程');
    } else {
        entries.forEach((entry, index) => {
            const title = scheduleField(entry, 'event') ?? '未命名日程';
            const start = formatScheduleDateTime(
                scheduleField(entry, 'start_time') ?? scheduleField(entry, 'original_start_time'),
                timeZone,
            );
            const end = formatScheduleDateTime(scheduleField(entry, 'end_time'), timeZone);
            const location = scheduleField(entry, 'location');
            const notes = scheduleField(entry, 'notes');
            const time = start === undefined ? undefined : end === undefined ? start : `${start} - ${end}`;
            const details = [
                time === undefined ? undefined : `时间：${time}`,
                location === undefined ? undefined : `地点：${location}`,
                notes === undefined ? undefined : `备注：${notes}`,
            ].filter((value): value is string => value !== undefined);
            lines.push(`${index + 1}. ${title}`);
            lines.push(...details.map((detail) => `   ${detail}`));
        });
    }
    if (intent.exceptions.length > 0) {
        lines.push(`例外调整：${intent.exceptions.length} 条`);
    }
    const queryDetails = [
        `查询条件：${formatQueryRange(intent.query.startDate, intent.query.endDate)}`,
        `状态：${QUERY_STATUS_LABELS[intent.query.status]}`,
        ...(intent.query.keyword === undefined ? [] : [`关键词：${intent.query.keyword}`]),
    ];
    lines.push(...queryDetails);
    return limitTemplateText(lines.join('\n'), MAX_SCHEDULE_QUERY_BODY_LENGTH);
}

function formatQueryRange(startDate: string | undefined, endDate: string | undefined): string {
    if (startDate !== undefined && endDate !== undefined) return `${startDate} 至 ${endDate}`;
    if (startDate !== undefined) return `${startDate} 起`;
    if (endDate !== undefined) return `截至 ${endDate}`;
    return '未限定日期';
}

function scheduleField(value: JsonValue, key: string): string | undefined {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) return undefined;
    const field = value[key];
    return typeof field === 'string' && field.trim() !== '' ? field.trim().replace(/\s+/gu, ' ') : undefined;
}

function formatScheduleDateTime(value: string | undefined, timeZone: string): string | undefined {
    if (value === undefined) return undefined;
    const localMatch = /^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2})(?::\d{2})?$/u.exec(value);
    if (localMatch !== null) {
        return `${Number(localMatch[1])}年${Number(localMatch[2])}月${Number(localMatch[3])}日 ${localMatch[4]}:${localMatch[5]}`;
    }
    const parsed = new Date(value);
    return Number.isNaN(parsed.getTime()) ? value : formatTemplateTime(value, timeZone);
}

function limitTemplateText(value: string, maxLength: number): string {
    const characters = [...value];
    return characters.length <= maxLength ? value : `${characters.slice(0, maxLength - 1).join('')}…`;
}

function templateField(value: string): string {
    const field = value.trim();
    if (!/^[A-Za-z][A-Za-z0-9_]{0,63}$/u.test(field)) {
        throw new ImGatewayError('invalid_contract', 'WeChat template field name is invalid');
    }
    return field;
}

function isAbortError(error: unknown): boolean {
    return error instanceof Error && error.name === 'AbortError';
}

function isNamedError(error: unknown, name: string): boolean {
    return error instanceof Error && error.name === name;
}
