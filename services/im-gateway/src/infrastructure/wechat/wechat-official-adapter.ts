import { createHash, timingSafeEqual } from 'node:crypto';

import { XMLParser, XMLValidator } from 'fast-xml-parser';

import type { NotificationIntent, ScheduleReceiptIntent } from '../../contracts/device-gateway.js';
import { unsafeId, type ChannelAccountId } from '../../contracts/ids.js';
import type { NormalizedImEvent } from '../../contracts/platform-events.js';
import type { PlatformCapabilityPort } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { IsoDateTime, JsonValue } from '../../shared/types.js';
import type { ChannelAccount, ChannelCapabilities } from '../../domain/models.js';

const MAX_WEBHOOK_BYTES = 64 * 1024;
const BINDING_CODE = /^(?:绑定|bind)\s*[:：]?\s*([0-9]{4,12})$/iu;
const XML_PARSER = new XMLParser({
    ignoreAttributes: true,
    parseTagValue: false,
    processEntities: true,
    trimValues: true,
});

/** 微信公众号 Webhook 请求的传输层表示。 */
export interface WechatWebhookRequest {
    readonly signature?: string;
    readonly timestamp?: string;
    readonly nonce?: string;
    readonly echostr?: string;
    readonly body?: string | Uint8Array;
    readonly xml?: string;
}

/** 创建微信公众号能力适配器所需的账号级配置。 */
export interface WechatOfficialAdapterOptions {
    readonly channelAccountId: ChannelAccountId;
    /** 微信公众平台后台配置的 Token；只能由部署配置注入。 */
    readonly token: string;
}

/** 微信公众号能力、Webhook 验签和入站事件归一化适配器。 */
export class WechatOfficialAdapter implements PlatformCapabilityPort {
    public readonly platform = 'wechat_official' as const;

    private readonly channelAccountId: ChannelAccountId;

    private readonly token: string;

    /** @param options 账号标识与部署注入的微信公众号 Token。 */
    public constructor(options: WechatOfficialAdapterOptions) {
        if (options.channelAccountId.trim() === '' || options.token === '') {
            throw new ImGatewayError('invalid_contract', 'WeChat adapter requires a channel account and token');
        }
        this.channelAccountId = options.channelAccountId;
        this.token = options.token;
    }

    /** {@inheritDoc PlatformCapabilityPort.capabilities} */
    public capabilities(_account: ChannelAccount): Promise<ChannelCapabilities> {
        return Promise.resolve({
            proactiveMessage: true,
            nativeAction: false,
            actionUi: true,
            deliveryReceipt: true,
            presentationTypes: ['template', 'text_with_action_ui'],
        });
    }

    /** {@inheritDoc PlatformCapabilityPort.renderScheduleReceipt} */
    public renderScheduleReceipt(intent: ScheduleReceiptIntent): Promise<JsonValue> {
        return Promise.resolve({ type: 'text', text: intent.summary });
    }

    /** {@inheritDoc PlatformCapabilityPort.renderNotification} */
    public renderNotification(intent: NotificationIntent): Promise<JsonValue> {
        return Promise.resolve({
            type: 'wechat_template',
            title: intent.content.title,
            reminderTriggerId: intent.reminderTriggerId,
        });
    }

    /**
     * 校验微信公众号服务器配置请求，并在成功时返回微信要求的 echostr。
     * @param request 微信 query 参数构成的请求。
     * @returns 验证请求的 echostr；普通 POST 请求没有 echostr 时返回 undefined。
     */
    public verifyWebhook(request: WechatWebhookRequest): string | undefined {
        const signature = requiredString(request.signature, 'signature');
        const timestamp = requiredString(request.timestamp, 'timestamp');
        const nonce = requiredString(request.nonce, 'nonce');
        if (!isSha1(signature) || !constantTimeEqual(signature, sha1([this.token, timestamp, nonce].sort().join('')))) {
            throw new ImGatewayError('invalid_contract', 'WeChat webhook signature is invalid');
        }
        return request.echostr;
    }

    /**
     * 校验并将微信公众号 XML webhook 转成平台无关的入站事件。
     * @param rawEvent 包含签名、时间戳、随机串和 XML body 的 HTTP 请求。
     * @returns 可交给 Gateway Application 的规范化事件。
     */
    public async normalizeInbound(rawEvent: unknown): Promise<NormalizedImEvent> {
        const request = asWebhookRequest(rawEvent);
        this.verifyWebhook(request);
        const xml = await readBody(request);
        const fields = parseWechatXml(xml);
        const externalUserId = requiredField(fields, 'FromUserName');
        const msgType = requiredField(fields, 'MsgType').toLowerCase();
        const occurredAt = eventTime(fields.CreateTime, request.timestamp);

        if (msgType === 'event') {
            return this.normalizeEvent(fields, externalUserId, occurredAt);
        }
        if (msgType === 'text') {
            return this.normalizeText(fields, externalUserId, occurredAt);
        }
        if (msgType === 'image' || msgType === 'voice' || msgType === 'video' || msgType === 'shortvideo') {
            const messageId = fields.MsgId;
            const externalEventId = messageId === undefined ? stableEventId(msgType, fields) : `message:${messageId}`;
            return {
                id: unsafeId(`wechat:${externalEventId}`),
                externalEventId,
                platform: this.platform,
                channelAccountId: this.channelAccountId,
                occurredAt,
                type: 'message.received',
                payload: {
                    externalUserId,
                    ...(messageId === undefined ? {} : { messageId }),
                    messageType: msgType,
                },
            };
        }
        throw new ImGatewayError('capability_not_supported', `Unsupported WeChat message type: ${msgType}`);
    }

    private normalizeText(
        fields: Record<string, string>,
        externalUserId: string,
        occurredAt: IsoDateTime,
    ): NormalizedImEvent {
        const text = requiredField(fields, 'Content');
        const messageId = fields.MsgId;
        const externalEventId = messageId === undefined ? stableEventId('text', fields) : `message:${messageId}`;
        const binding = BINDING_CODE.exec(text.trim());
        if (binding !== null) {
            return {
                id: unsafeId(`wechat:${externalEventId}`),
                externalEventId,
                platform: this.platform,
                channelAccountId: this.channelAccountId,
                occurredAt,
                type: 'binding.requested',
                payload: { displayCode: binding[1]!, externalUserId },
            };
        }
        return {
            id: unsafeId(`wechat:${externalEventId}`),
            externalEventId,
            platform: this.platform,
            channelAccountId: this.channelAccountId,
            occurredAt,
            type: 'message.received',
            payload: {
                externalUserId,
                ...(messageId === undefined ? {} : { messageId }),
                text,
            },
        };
    }

    private normalizeEvent(
        fields: Record<string, string>,
        externalUserId: string,
        occurredAt: IsoDateTime,
    ): NormalizedImEvent {
        const eventName = requiredField(fields, 'Event').toLowerCase();
        if (eventName === 'templatesendjobfinish') {
            const messageId = fields.MsgID ?? fields.MsgId;
            if (messageId === undefined) {
                throw new ImGatewayError('invalid_contract', 'WeChat template callback is missing MsgID');
            }
            const status = requiredField(fields, 'Status');
            const externalEventId = `template:${messageId}:${status.toLowerCase()}`;
            const stage = status.toLowerCase() === 'success' ? 'delivered' : 'failed';
            const receipt = {
                externalEventId,
                channelAccountId: this.channelAccountId,
                externalMessageId: messageId,
                dedupeKey: `wechat:${externalEventId}`,
                stage,
                occurredAt,
                platformCode: status,
            } as const;
            return {
                id: unsafeId(`wechat:${externalEventId}`),
                externalEventId,
                platform: this.platform,
                channelAccountId: this.channelAccountId,
                occurredAt,
                type: 'delivery.updated',
                payload: receipt,
            };
        }

        const externalEventId =
            fields.MsgId === undefined
                ? stableEventId(`event:${eventName}`, fields)
                : `event:${eventName}:${fields.MsgId}`;
        const event =
            eventName === 'subscribe' ? 'subscribed' : eventName === 'unsubscribe' ? 'unsubscribed' : eventName;
        return {
            id: unsafeId(`wechat:${externalEventId}`),
            externalEventId,
            platform: this.platform,
            channelAccountId: this.channelAccountId,
            occurredAt,
            type: 'message.received',
            payload: { externalUserId, event },
        };
    }
}

/** 兼容旧命名：微信公众号能力适配器。 */
export const WechatCapabilityAdapter = WechatOfficialAdapter;

/** @deprecated 使用 {@link WechatOfficialAdapter}；保留旧导出避免破坏已有装配代码。 */
export class WechatCapabilityStub extends WechatOfficialAdapter {
    private readonly legacyStub: boolean;

    /** @param options 账号标识与部署注入的微信公众号 Token。 */
    public constructor(options?: WechatOfficialAdapterOptions) {
        super(
            options ?? {
                channelAccountId: unsafeId<ChannelAccountId>('legacy-wechat-stub'),
                token: 'legacy-wechat-stub-token',
            },
        );
        this.legacyStub = options === undefined;
    }

    /** {@inheritDoc PlatformCapabilityPort.normalizeInbound} */
    public override normalizeInbound(rawEvent: unknown): Promise<NormalizedImEvent> {
        if (this.legacyStub) {
            void rawEvent;
            return Promise.reject(
                new ImGatewayError('not_implemented', 'Use WechatOfficialAdapter for configured WeChat webhooks'),
            );
        }
        return super.normalizeInbound(rawEvent);
    }
}

function asWebhookRequest(value: unknown): WechatWebhookRequest {
    if (!isRecord(value)) throw new ImGatewayError('invalid_contract', 'WeChat webhook request must be an object');
    const query = isRecord(value.query) ? value.query : value;
    const signature = stringValue(query.signature);
    const timestamp = stringValue(query.timestamp);
    const nonce = stringValue(query.nonce);
    const echostr = stringValue(query.echostr);
    const body = typeof value.body === 'string' || value.body instanceof Uint8Array ? value.body : undefined;
    const xml = stringValue(value.xml);
    return {
        ...(signature === undefined ? {} : { signature }),
        ...(timestamp === undefined ? {} : { timestamp }),
        ...(nonce === undefined ? {} : { nonce }),
        ...(echostr === undefined ? {} : { echostr }),
        ...(body === undefined ? {} : { body }),
        ...(xml === undefined ? {} : { xml }),
    };
}

async function readBody(request: WechatWebhookRequest): Promise<string> {
    if (request.body instanceof Uint8Array) {
        if (request.body.byteLength > MAX_WEBHOOK_BYTES)
            throw new ImGatewayError('invalid_contract', 'WeChat webhook is too large');
        return new TextDecoder().decode(request.body);
    }
    const body = request.body ?? request.xml;
    if (body === undefined) throw new ImGatewayError('invalid_contract', 'WeChat webhook body is missing');
    if (new TextEncoder().encode(body).byteLength > MAX_WEBHOOK_BYTES) {
        throw new ImGatewayError('invalid_contract', 'WeChat webhook is too large');
    }
    return body;
}

function parseWechatXml(xml: string): Record<string, string> {
    if (/<!(?:DOCTYPE|ENTITY)\b/iu.test(xml)) {
        throw new ImGatewayError('invalid_contract', 'WeChat webhook XML envelope is invalid');
    }
    if (XMLValidator.validate(xml) !== true) {
        throw new ImGatewayError('invalid_contract', 'WeChat webhook XML is malformed');
    }
    let parsed: unknown;
    try {
        parsed = XML_PARSER.parse(xml);
    } catch {
        throw new ImGatewayError('invalid_contract', 'WeChat webhook XML is malformed');
    }
    if (!isRecord(parsed)) {
        throw new ImGatewayError('invalid_contract', 'WeChat webhook XML envelope is invalid');
    }
    const roots = Object.keys(parsed).filter((name) => name !== '?xml');
    if (roots.length !== 1 || roots[0] !== 'xml' || !isRecord(parsed.xml)) {
        throw new ImGatewayError('invalid_contract', 'WeChat webhook XML envelope is invalid');
    }
    const fields: Record<string, string> = {};
    for (const [name, value] of Object.entries(parsed.xml)) {
        if (!/^[A-Za-z][A-Za-z0-9_]*$/u.test(name) || typeof value !== 'string') {
            throw new ImGatewayError('invalid_contract', 'WeChat webhook XML contains an invalid field');
        }
        fields[name] = value;
    }
    if (Object.keys(fields).length === 0) {
        throw new ImGatewayError('invalid_contract', 'WeChat webhook XML contains no fields');
    }
    return fields;
}

function requiredString(value: string | undefined, field: string): string {
    if (value === undefined || value.trim() === '')
        throw new ImGatewayError('invalid_contract', `WeChat ${field} is missing`);
    return value;
}

function requiredField(fields: Record<string, string>, field: string): string {
    return requiredString(fields[field], field);
}

function stringValue(value: unknown): string | undefined {
    return typeof value === 'string' ? value : undefined;
}

function isRecord(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function isSha1(value: string): boolean {
    return /^[0-9a-f]{40}$/iu.test(value);
}

function constantTimeEqual(left: string, right: string): boolean {
    if (left.length !== right.length) return false;
    return timingSafeEqual(new TextEncoder().encode(left), new TextEncoder().encode(right));
}

function sha1(value: string): string {
    // SHA-1 is mandated by the WeChat webhook protocol; the token remains secret and is never logged.
    return createHash('sha1').update(value, 'utf8').digest('hex');
}

function eventTime(createTime: string | undefined, requestTimestamp: string | undefined): IsoDateTime {
    const seconds = Number(createTime ?? requestTimestamp);
    const milliseconds = seconds * 1000;
    if (!Number.isSafeInteger(seconds) || seconds < 0 || !Number.isFinite(milliseconds)) {
        throw new ImGatewayError('invalid_contract', 'WeChat event timestamp is invalid');
    }
    const date = new Date(milliseconds);
    if (Number.isNaN(date.getTime())) {
        throw new ImGatewayError('invalid_contract', 'WeChat event timestamp is invalid');
    }
    return date.toISOString() as IsoDateTime;
}

function stableEventId(kind: string, fields: Record<string, string>): string {
    const canonicalFields = Object.entries(fields)
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([key, value]) => `${key.length}:${key}${value.length}:${value}`)
        .join('');
    return `${kind}:${sha1(canonicalFields)}`;
}
