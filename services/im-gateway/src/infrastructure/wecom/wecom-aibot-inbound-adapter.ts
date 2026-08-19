import type { NotificationIntent, ScheduleReceiptIntent } from '../../contracts/device-gateway.js';
import { unsafeId, type ChannelAccountId } from '../../contracts/ids.js';
import type { NormalizedImEvent } from '../../contracts/platform-events.js';
import type { PlatformCapabilityPort } from '../../ports/external.js';
import type { ChannelAccount, ChannelCapabilities } from '../../domain/models.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { IsoDateTime, JsonValue } from '../../shared/types.js';

const BINDING_CODE = /^(?:绑定|bind)\s*[:：]?\s*([0-9]{6})$/iu;
const MAX_EXTERNAL_ID_LENGTH = 512;
const MAX_TEXT_LENGTH = 16 * 1024;

/** 创建企业微信 AI Bot 入站适配器所需的账号级配置。 */
export interface WecomAibotInboundAdapterOptions {
    readonly channelAccountId: ChannelAccountId;
    readonly botId: string;
    /** 可替换的接收时间来源，仅在平台帧未提供 create_time 时使用。 */
    readonly now?: () => IsoDateTime;
}

/**
 * 将企业微信 AI Bot URL 回调解密后的单聊文本消息归一化为 Gateway 入站事件。
 *
 * 此适配器只实现入站绑定链路；主动投递由后续渠道切片提供。
 */
export class WecomAibotInboundAdapter implements PlatformCapabilityPort {
    public readonly platform = 'wecom_aibot' as const;

    private readonly channelAccountId: ChannelAccountId;

    private readonly botId: string;

    private readonly now: () => IsoDateTime;

    /**
     * @param options 渠道账号与企业微信机器人标识。
     */
    public constructor(options: WecomAibotInboundAdapterOptions) {
        this.channelAccountId = requiredOption(options.channelAccountId, 'channel account ID') as ChannelAccountId;
        this.botId = requiredOption(options.botId, 'bot ID');
        this.now = options.now ?? (() => new Date().toISOString() as IsoDateTime);
    }

    /** {@inheritDoc PlatformCapabilityPort.capabilities} */
    public capabilities(account: ChannelAccount): Promise<ChannelCapabilities> {
        if (account.id !== this.channelAccountId || account.platform !== this.platform || account.status !== 'active') {
            return Promise.resolve(unavailableCapabilities());
        }
        return Promise.resolve(unavailableCapabilities());
    }

    /** {@inheritDoc PlatformCapabilityPort.renderScheduleReceipt} */
    public renderScheduleReceipt(intent: ScheduleReceiptIntent): Promise<JsonValue> {
        void intent;
        return Promise.reject(
            new ImGatewayError('capability_not_supported', 'WeCom AI Bot outbound delivery is not configured'),
        );
    }

    /** {@inheritDoc PlatformCapabilityPort.renderNotification} */
    public renderNotification(intent: NotificationIntent): Promise<JsonValue> {
        void intent;
        return Promise.reject(
            new ImGatewayError('capability_not_supported', 'WeCom AI Bot outbound delivery is not configured'),
        );
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
