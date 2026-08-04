import type {
    ActionId,
    BindingId,
    ChannelAccountId,
    DeliveryAttemptId,
    DeliveryId,
    DeliveryReceiptId,
    DeviceId,
    ExternalIdentityId,
    OperationId,
    OutboxEventId,
    PairingSessionId,
    ReminderTriggerId,
    RequestId,
} from '../contracts/ids.js';
import type { NotificationIntent, ReminderActionCommand, ScheduleReceiptIntent } from '../contracts/device-gateway.js';
import type { NormalizedImEvent } from '../contracts/platform-events.js';
import type {
    ChannelAccount,
    ChannelCapabilities,
    ConversationRef,
    Delivery,
    ExternalIdentity,
} from '../domain/models.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';

export interface Clock {
    now(): IsoDateTime;
    addMinutes(value: IsoDateTime, minutes: number): IsoDateTime;
}

export interface IdGenerator {
    nextChannelAccountId(): ChannelAccountId;
    nextPairingSessionId(): PairingSessionId;
    nextBindingId(): BindingId;
    nextExternalIdentityId(): ExternalIdentityId;
    nextDeliveryId(): DeliveryId;
    nextDeliveryAttemptId(): DeliveryAttemptId;
    nextDeliveryReceiptId(): DeliveryReceiptId;
    actionIdForDelivery(deliveryId: DeliveryId): ActionId;
    nextOperationId(): OperationId;
    nextOutboxEventId(): OutboxEventId;
    nextRequestId(): RequestId;
}

export interface ProtectedExternalIdentity {
    readonly ciphertext: string;
    readonly hash: string;
}

export interface ExternalIdentityProtector {
    protect(plainExternalUserId: string): Promise<ProtectedExternalIdentity>;
}

export interface PairingCodePort {
    issue(): Promise<{ readonly displayCode: string; readonly hash: string }>;
    hash(displayCode: string): Promise<string>;
}

export interface ChannelCapabilityResolver {
    resolve(account: ChannelAccount): Promise<ChannelCapabilities>;
}

export interface ChannelHealth {
    readonly accountId: ChannelAccountId;
    readonly status: 'healthy' | 'degraded' | 'unavailable';
    readonly checkedAt: IsoDateTime;
    readonly detail?: string;
}

export interface ChannelHealthPort {
    check(account: ChannelAccount): Promise<ChannelHealth>;
}

export interface ConversationResolverPort {
    resolveDirect(identity: ExternalIdentity): Promise<ConversationRef>;
}

export interface OutboundImMessage {
    readonly delivery: Delivery;
    readonly conversation: ConversationRef;
    readonly content: JsonValue;
}

export interface ImSendAcceptance {
    readonly accepted: boolean;
    readonly platformMessageId?: string;
    readonly retryable?: boolean;
    readonly errorCode?: string;
}

export interface ImChannelPort {
    send(message: OutboundImMessage): Promise<ImSendAcceptance>;
}

export interface DeliveryRendererPort {
    render(
        delivery: Delivery,
        account: ChannelAccount,
        capabilities: ChannelCapabilities,
        context: {
            readonly actionToken?: string;
        },
    ): Promise<JsonValue>;
}

export interface PlatformCapabilityPort {
    readonly platform: ChannelAccount['platform'];
    capabilities(account: ChannelAccount): Promise<ChannelCapabilities>;
    renderScheduleReceipt(intent: ScheduleReceiptIntent): Promise<JsonValue>;
    renderNotification(intent: NotificationIntent): Promise<JsonValue>;
    normalizeInbound(rawEvent: unknown): Promise<NormalizedImEvent>;
}

export interface ActionStreamSubscription {
    readonly deviceId: DeviceId;
    readonly reminderTriggerId: ReminderTriggerId;
    /** Server-derived deadline; never accepted from the device request. */
    readonly expiresAt: IsoDateTime;
    readonly lastEventId?: ActionId;
    readonly signal?: AbortSignal;
}

export interface ActionCommandStreamPort {
    publish(command: ReminderActionCommand): Promise<void>;
    subscribe(subscription: ActionStreamSubscription): AsyncIterable<ReminderActionCommand>;
    close(actionId: ActionId): Promise<void>;
}

export interface ActionTokenClaims {
    readonly actionId: ActionId;
    readonly deliveryId: DeliveryId;
    readonly expiresAt: IsoDateTime;
}

export interface ActionTokenPort {
    issue(claims: ActionTokenClaims): Promise<string>;
    verify(token: string): Promise<ActionTokenClaims>;
    fingerprint(token: string): Promise<string>;
}

export interface DevicePrincipal {
    readonly deviceId: DeviceId;
}

export interface DeviceAuthenticationPort {
    authenticate(authorization: string): Promise<DevicePrincipal>;
}
