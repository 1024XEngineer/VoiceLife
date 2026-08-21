import { createHash } from 'node:crypto';

import type { NotificationIntent, ScheduleReceiptIntent } from '../../contracts/device-gateway.js';
import { parseNotificationIntent, parseScheduleReceiptIntent } from '../../contracts/device-gateway-parser.js';
import { unsafeId, type ChannelAccountId, type ExternalIdentityId } from '../../contracts/ids.js';
import type { NormalizedImEvent } from '../../contracts/platform-events.js';
import type { PlatformCapabilityPort } from '../../ports/external.js';
import type {
    ChannelCapabilityResolver,
    DeliveryRendererPort,
    ImChannelPort,
    ImSendAcceptance,
    OutboundImMessage,
} from '../../ports/external.js';
import type { ChannelAccount, ChannelCapabilities, Delivery } from '../../domain/models.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { IsoDateTime, JsonValue } from '../../shared/types.js';

const BINDING_CODE = /^(?:绑定|bind)\s*[:：]?\s*([0-9]{6})$/iu;
const ACTION_KEY = /^voicelife-action:v1:([^:]+):(acknowledge|snooze):(\d*)$/u;
const MAX_EXTERNAL_ID_LENGTH = 512;
const MAX_TEXT_LENGTH = 16 * 1024;

/** WSS 长连接出站 Markdown 所需的最小传输接口。 */
export interface WecomAibotMarkdownTransport {
    /**
     * @param chatId 企业微信单聊标识。
     * @param content UTF-8 Markdown 正文。
     * @returns 企业微信对本次发送的即时受理或失败分类。
     */
    sendMarkdown(chatId: string, content: string): Promise<ImSendAcceptance>;
}

/** WSS 长连接模板卡片出站所需的最小传输接口。 */
export interface WecomAibotTemplateCardTransport {
    /**
     * @param chatId 企业微信单聊标识。
     * @param card 企业微信模板卡片结构，不包含 chatid 和 msgtype 外层字段。
     * @returns 企业微信对本次发送的即时受理或失败分类。
     */
    sendTemplateCard(chatId: string, card: JsonValue): Promise<ImSendAcceptance>;
}

/** 部署组合根注入的企业微信 WSS 出站依赖。 */
export interface WecomAibotOutboundOptions {
    /** 将持久化的受保护用户标识还原为企业微信单聊标识；实现不得记录明文。 */
    readonly revealExternalUserId: (ciphertext: string) => Promise<string> | string;
    /** 已订阅 WSS 连接的发送端口。 */
    readonly transport: WecomAibotMarkdownTransport & Partial<WecomAibotTemplateCardTransport>;
}

/** 创建企业微信 AI Bot 入站适配器所需的账号级配置。 */
export interface WecomAibotInboundAdapterOptions {
    readonly channelAccountId: ChannelAccountId;
    readonly botId: string;
    /** 可替换的接收时间来源，仅在平台帧未提供 create_time 时使用。 */
    readonly now?: () => IsoDateTime;
    /** 将平台用户映射为内部身份标识，用于动作事件的绑定校验。 */
    readonly resolveExternalIdentityId?: (externalUserId: string) => Promise<ExternalIdentityId | undefined>;
    /** 可选 WSS 出站依赖；缺省时 Adapter 只接收入站消息。 */
    readonly outbound?: WecomAibotOutboundOptions;
}

/**
 * 将企业微信 AI Bot WSS 回调的单聊文本消息归一化为 Gateway 入站事件。
 *
 * 没有注入 WSS 出站端口时，此适配器仍可独立处理入站绑定链路。
 */
export class WecomAibotInboundAdapter
    implements PlatformCapabilityPort, ChannelCapabilityResolver, DeliveryRendererPort, ImChannelPort
{
    public readonly platform = 'wecom_aibot' as const;

    private readonly channelAccountId: ChannelAccountId;

    private readonly botId: string;

    private readonly now: () => IsoDateTime;

    private readonly outbound: WecomAibotOutboundOptions | undefined;

    private readonly resolveExternalIdentityId: WecomAibotInboundAdapterOptions['resolveExternalIdentityId'];

    /**
     * @param options 渠道账号与企业微信机器人标识。
     */
    public constructor(options: WecomAibotInboundAdapterOptions) {
        this.channelAccountId = requiredOption(options.channelAccountId, 'channel account ID') as ChannelAccountId;
        this.botId = requiredOption(options.botId, 'bot ID');
        this.now = options.now ?? (() => new Date().toISOString() as IsoDateTime);
        this.outbound = options.outbound;
        this.resolveExternalIdentityId = options.resolveExternalIdentityId;
    }

    /** {@inheritDoc PlatformCapabilityPort.capabilities} */
    public capabilities(account: ChannelAccount): Promise<ChannelCapabilities> {
        if (account.id !== this.channelAccountId || account.platform !== this.platform || account.status !== 'active') {
            return Promise.resolve(unavailableCapabilities());
        }
        return Promise.resolve(
            this.outbound === undefined
                ? unavailableCapabilities()
                : outboundCapabilities(this.outbound.transport.sendTemplateCard !== undefined),
        );
    }

    /** {@inheritDoc ChannelCapabilityResolver.resolve} */
    public resolve(account: ChannelAccount): Promise<ChannelCapabilities> {
        return this.capabilities(account);
    }

    /** {@inheritDoc PlatformCapabilityPort.renderScheduleReceipt} */
    public renderScheduleReceipt(intent: ScheduleReceiptIntent): Promise<JsonValue> {
        if (this.outbound === undefined) return outboundUnavailable();
        return Promise.resolve(markdownPayload('日程已更新', intent.summary));
    }

    /** {@inheritDoc PlatformCapabilityPort.renderNotification} */
    public renderNotification(intent: NotificationIntent): Promise<JsonValue> {
        if (this.outbound === undefined) return outboundUnavailable();
        return Promise.resolve(markdownPayload(intent.content.title, intent.content.body));
    }

    /** {@inheritDoc DeliveryRendererPort.render} */
    public render(
        delivery: Delivery,
        account: ChannelAccount,
        capabilities: ChannelCapabilities,
        context: { readonly actionToken?: string },
    ): Promise<JsonValue> {
        if (
            delivery.presentationType === 'native_card' &&
            this.outbound !== undefined &&
            account.id === this.channelAccountId &&
            account.platform === this.platform &&
            capabilities.presentationTypes.includes('native_card') &&
            context.actionToken !== undefined
        ) {
            return Promise.resolve(
                templateCardPayloadFromNotification(
                    parseNotificationIntent(delivery.semanticPayload),
                    context.actionToken,
                ),
            );
        }
        if (delivery.presentationType === 'native_card') return outboundUnavailable();
        if (
            this.outbound === undefined ||
            delivery.channelAccountId !== this.channelAccountId ||
            account.id !== this.channelAccountId ||
            account.platform !== this.platform ||
            delivery.presentationType !== 'rich_text' ||
            !capabilities.presentationTypes.includes(delivery.presentationType)
        ) {
            return outboundUnavailable();
        }
        return Promise.resolve(
            delivery.kind === 'schedule_receipt'
                ? markdownPayloadFromScheduleReceipt(parseScheduleReceiptIntent(delivery.semanticPayload))
                : markdownPayloadFromNotification(parseNotificationIntent(delivery.semanticPayload)),
        );
    }

    /** {@inheritDoc ImChannelPort.send} */
    public async send(message: OutboundImMessage): Promise<ImSendAcceptance> {
        if (this.outbound === undefined) {
            return { accepted: false, retryable: false, errorCode: 'wecom_aibot_not_configured' };
        }
        if (message.delivery.channelAccountId !== this.channelAccountId) {
            return { accepted: false, retryable: false, errorCode: 'wecom_aibot_account_mismatch' };
        }
        const content = markdownContent(message.content);
        const templateCard = templateCardContent(message.content);
        if (content === undefined && templateCard === undefined) {
            return { accepted: false, retryable: false, errorCode: 'wecom_aibot_invalid_message' };
        }
        const externalUserId = await this.outbound.revealExternalUserId(
            message.conversation.externalConversationIdCiphertext,
        );
        if (templateCard !== undefined) {
            if (this.outbound.transport.sendTemplateCard === undefined) {
                return { accepted: false, retryable: false, errorCode: 'wecom_aibot_invalid_message' };
            }
            return this.outbound.transport.sendTemplateCard(externalUserId, templateCard);
        }
        if (content === undefined) {
            return { accepted: false, retryable: false, errorCode: 'wecom_aibot_invalid_message' };
        }
        return this.outbound.transport.sendMarkdown(externalUserId, content);
    }

    /** {@inheritDoc PlatformCapabilityPort.normalizeInbound} */
    public async normalizeInbound(rawEvent: unknown): Promise<NormalizedImEvent> {
        const body = requiredRecord(rawEvent, 'WeCom AI Bot callback');
        if (requiredString(body, 'aibotid', 'WeCom AI Bot ID') !== this.botId) {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback targets another bot');
        }
        if (requiredString(body, 'chattype', 'WeCom AI Bot chat type') !== 'single') {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot group chat is not supported');
        }
        if (body.chatid !== undefined) {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot group chat is not supported');
        }
        const externalEventId = requiredExternalId(body, 'msgid', 'WeCom AI Bot message ID');
        const from = requiredRecord(body.from, 'WeCom AI Bot sender');
        const externalUserId = requiredExternalId(from, 'userid', 'WeCom AI Bot userid');
        const occurredAt = body.create_time === undefined ? this.now() : eventTime(body.create_time);
        const messageType = requiredString(body, 'msgtype', 'WeCom AI Bot message type').toLowerCase();
        if (messageType === 'event') {
            return this.actionEvent(body, externalEventId, externalUserId, occurredAt);
        }
        if (messageType !== 'text') {
            throw new ImGatewayError('capability_not_supported', 'WeCom AI Bot only supports text messages');
        }
        const text = requiredString(
            requiredRecord(body.text, 'WeCom AI Bot text'),
            'content',
            'WeCom AI Bot text content',
        );
        if (text.length > MAX_TEXT_LENGTH) {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot text content is too large');
        }
        const binding = BINDING_CODE.exec(text.trim());
        return binding === null
            ? this.messageEvent(externalEventId, externalUserId, occurredAt, text)
            : this.bindingEvent(externalEventId, externalUserId, occurredAt, binding[1]!);
    }

    private async actionEvent(
        body: Record<string, unknown>,
        externalEventId: string,
        externalUserId: string,
        occurredAt: IsoDateTime,
    ): Promise<NormalizedImEvent> {
        const event = requiredRecord(body.event, 'WeCom AI Bot event');
        if (requiredString(event, 'eventtype', 'WeCom AI Bot event type') !== 'template_card_event') {
            throw new ImGatewayError('capability_not_supported', 'WeCom AI Bot event type is not supported');
        }
        const cardEvent =
            event.template_card_event === undefined
                ? event
                : requiredRecord(event.template_card_event, 'WeCom AI Bot template card event');
        const key = firstString(cardEvent, ['event_key', 'button_key', 'eventkey', 'key']);
        if (key === undefined) {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot template card event key is required');
        }
        const action = parseActionKey(key);
        if (action === undefined) {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot template card event key is invalid');
        }
        if (this.resolveExternalIdentityId === undefined) {
            throw new ImGatewayError(
                'capability_not_supported',
                'WeCom AI Bot action identity resolution is not configured',
            );
        }
        const externalIdentityId = await this.resolveExternalIdentityId(externalUserId);
        if (externalIdentityId === undefined) {
            throw new ImGatewayError('action_expired', 'WeCom AI Bot action identity is not bound');
        }
        return {
            id: this.eventId(externalEventId),
            externalEventId,
            platform: this.platform,
            channelAccountId: this.channelAccountId,
            ...(externalIdentityId === undefined ? {} : { externalIdentityId }),
            occurredAt,
            type: 'action.triggered',
            payload: action,
        };
    }

    private bindingEvent(
        externalEventId: string,
        externalUserId: string,
        occurredAt: IsoDateTime,
        displayCode: string,
    ): NormalizedImEvent {
        return {
            id: this.eventId(externalEventId),
            externalEventId,
            platform: this.platform,
            channelAccountId: this.channelAccountId,
            occurredAt,
            type: 'binding.requested',
            payload: { displayCode, externalUserId },
        };
    }

    private messageEvent(
        externalEventId: string,
        externalUserId: string,
        occurredAt: IsoDateTime,
        text: string,
    ): NormalizedImEvent {
        return {
            id: this.eventId(externalEventId),
            externalEventId,
            platform: this.platform,
            channelAccountId: this.channelAccountId,
            occurredAt,
            type: 'message.received',
            payload: { externalUserId, messageType: 'text', text },
        };
    }

    private eventId(externalEventId: string): NormalizedImEvent['id'] {
        return unsafeId<NormalizedImEvent['id']>(`${this.channelAccountId}:wecom:${externalEventId}`);
    }
}

function unavailableCapabilities(): ChannelCapabilities {
    return {
        proactiveMessage: false,
        nativeAction: false,
        actionUi: false,
        deliveryReceipt: false,
        presentationTypes: [],
    };
}

function outboundCapabilities(nativeCard: boolean): ChannelCapabilities {
    return {
        proactiveMessage: true,
        nativeAction: nativeCard,
        actionUi: false,
        deliveryReceipt: false,
        presentationTypes: nativeCard ? ['native_card', 'rich_text'] : ['rich_text'],
    };
}

function outboundUnavailable(): Promise<never> {
    return Promise.reject(
        new ImGatewayError('capability_not_supported', 'WeCom AI Bot outbound delivery is not configured'),
    );
}

function markdownPayload(title: string, body: string | undefined): JsonValue {
    const content = [`**${title.trim()}**`, body?.trim() ?? ''].filter((part) => part !== '').join('\n');
    return { type: 'wecom_aibot_markdown', content };
}

function markdownPayloadFromScheduleReceipt(intent: ScheduleReceiptIntent): JsonValue {
    return markdownPayload('日程已更新', intent.summary);
}

function markdownPayloadFromNotification(intent: NotificationIntent): JsonValue {
    return markdownPayload(intent.content.title, intent.content.body);
}

function templateCardPayloadFromNotification(intent: NotificationIntent, actionToken: string): JsonValue {
    if (intent.reminderType !== 'strong' || intent.actions.length === 0) {
        throw new ImGatewayError('capability_not_supported', 'WeCom AI Bot native cards require a strong reminder');
    }
    const taskId = `voicelife-${shortTokenFingerprint(actionToken)}`;
    return {
        type: 'wecom_aibot_template_card',
        template_card: {
            card_type: 'button_interaction',
            main_title: { title: intent.content.title.trim() },
            ...(intent.content.body?.trim() === '' || intent.content.body === undefined
                ? {}
                : { sub_title_text: intent.content.body.trim() }),
            button_list: intent.actions.map((option) => ({
                text: option.label,
                style: option.type === 'acknowledge' ? 1 : 2,
                key: actionKey(actionToken, option.type, option.params?.minutes),
            })),
            task_id: taskId,
        },
    };
}

function templateCardContent(content: JsonValue): JsonValue | undefined {
    if (content === null || typeof content !== 'object' || Array.isArray(content)) return undefined;
    const value = content as Record<string, unknown>;
    return value.type === 'wecom_aibot_template_card' && isJsonRecord(value.template_card)
        ? (value.template_card as JsonValue)
        : undefined;
}

function actionKey(token: string, action: string, minutes: number | undefined): string {
    return `voicelife-action:v1:${token}:${action}:${minutes === undefined ? '' : String(minutes)}`;
}

function shortTokenFingerprint(token: string): string {
    return createHash('sha256').update(token, 'utf8').digest('hex').slice(0, 24);
}

function isJsonRecord(value: unknown): value is Record<string, unknown> {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function markdownContent(content: JsonValue): string | undefined {
    if (content === null || typeof content !== 'object' || Array.isArray(content)) return undefined;
    const value = content as Record<string, unknown>;
    return value.type === 'wecom_aibot_markdown' && typeof value.content === 'string' && value.content.trim() !== ''
        ? value.content
        : undefined;
}

function requiredOption(value: string, label: string): string {
    const normalized = value.trim();
    if (normalized === '') throw new ImGatewayError('invalid_contract', `WeCom AI Bot requires a ${label}`);
    return normalized;
}

function requiredRecord(value: unknown, label: string): Record<string, unknown> {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) {
        throw new ImGatewayError('invalid_contract', `${label} must be an object`);
    }
    return value as Record<string, unknown>;
}

function requiredString(record: Record<string, unknown>, key: string, label: string): string {
    const value = record[key];
    if (typeof value !== 'string' || value.trim() === '') {
        throw new ImGatewayError('invalid_contract', `${label} must be a non-empty string`);
    }
    return value;
}

function requiredExternalId(record: Record<string, unknown>, key: string, label: string): string {
    const value = requiredString(record, key, label).trim();
    if (value.length > MAX_EXTERNAL_ID_LENGTH || containsControlCharacter(value)) {
        throw new ImGatewayError('invalid_contract', `${label} is invalid`);
    }
    return value;
}

function firstString(record: Record<string, unknown>, keys: readonly string[]): string | undefined {
    for (const key of keys) {
        if (typeof record[key] === 'string' && record[key].trim() !== '') return record[key] as string;
    }
    return undefined;
}

function parseActionKey(key: string):
    | {
          readonly token: string;
          readonly action: 'acknowledge' | 'snooze';
          readonly params?: { readonly minutes: number };
      }
    | undefined {
    const match = ACTION_KEY.exec(key);
    if (match === null || match[1] === undefined || match[2] === undefined || match[3] === undefined) return undefined;
    if (match[2] === 'acknowledge') {
        return { token: match[1], action: 'acknowledge' };
    }
    const minutes = Number(match[3]);
    return Number.isSafeInteger(minutes) && minutes > 0
        ? { token: match[1], action: 'snooze', params: { minutes } }
        : undefined;
}

function containsControlCharacter(value: string): boolean {
    for (const character of value) {
        const codePoint = character.codePointAt(0);
        if (codePoint !== undefined && (codePoint <= 0x1f || codePoint === 0x7f)) return true;
    }
    return false;
}

function eventTime(value: unknown): IsoDateTime {
    const seconds =
        typeof value === 'number'
            ? value
            : typeof value === 'string' && /^\d{1,12}$/u.test(value)
              ? Number(value)
              : Number.NaN;
    if (!Number.isSafeInteger(seconds) || seconds <= 0) {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot create_time must be Unix seconds');
    }
    return new Date(seconds * 1000).toISOString() as IsoDateTime;
}
