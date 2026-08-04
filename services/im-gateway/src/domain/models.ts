import type {
    ActionId,
    BindingId,
    ChannelAccountId,
    CorrelationId,
    DeliveryAttemptId,
    DeliveryId,
    DeliveryReceiptId,
    DeviceId,
    EventId,
    ExternalIdentityId,
    InboundEventId,
    OperationId,
    OutboxEventId,
    PairingSessionId,
    ReminderTriggerId,
    RequestId,
    UserId,
} from '../contracts/ids.js';
import type { NotificationSubmission, ReminderActionKind, ReminderActionResult } from '../contracts/device-gateway.js';
import type { ImPlatform } from '../contracts/platform-events.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';

export interface ChannelCapabilities {
    readonly proactiveMessage: boolean;
    readonly nativeAction: boolean;
    readonly actionUi: boolean;
    readonly deliveryReceipt: boolean;
    readonly presentationTypes: readonly PresentationType[];
}

export type PresentationType = 'native_card' | 'template' | 'rich_text' | 'text_with_action_ui';

export interface ChannelAccount {
    readonly id: ChannelAccountId;
    readonly platform: ImPlatform;
    readonly tenantExternalId: string;
    readonly koishiBotId: string;
    readonly credentialRef: string;
    readonly connectionMode: 'webhook' | 'websocket' | 'both';
    readonly capabilityConfig?: JsonValue;
    readonly status: 'active' | 'disabled' | 'error';
    readonly createdAt: IsoDateTime;
    readonly updatedAt: IsoDateTime;
}

export interface PairingSession {
    readonly id: PairingSessionId;
    readonly displayCodeHash: string;
    readonly userId?: UserId;
    readonly deviceId: DeviceId;
    readonly allowedPlatforms?: readonly ImPlatform[];
    readonly status: 'pending' | 'confirmed' | 'expired' | 'cancelled';
    readonly expiresAt: IsoDateTime;
    readonly createdAt: IsoDateTime;
    readonly confirmedAt?: IsoDateTime;
}

export interface ExternalIdentity {
    readonly id: ExternalIdentityId;
    readonly channelAccountId: ChannelAccountId;
    readonly externalUserIdCiphertext: string;
    readonly externalUserIdHash: string;
    readonly displayName?: string;
    readonly status: 'active' | 'unreachable' | 'revoked';
    readonly createdAt: IsoDateTime;
    readonly updatedAt: IsoDateTime;
}

export interface ConversationRef {
    readonly channelAccountId: ChannelAccountId;
    readonly externalIdentityId: ExternalIdentityId;
    readonly kind: 'direct' | 'group';
    readonly externalConversationIdCiphertext: string;
}

export interface ImBinding {
    readonly id: BindingId;
    readonly userId: UserId;
    readonly deviceId?: DeviceId;
    readonly externalIdentityId: ExternalIdentityId;
    readonly priority: number;
    readonly status: 'active' | 'unbound' | 'revoked';
    readonly boundAt: IsoDateTime;
    readonly unboundAt?: IsoDateTime;
    readonly revokedAt?: IsoDateTime;
}

export interface InboundEventRecord {
    readonly id: InboundEventId;
    readonly channelAccountId: ChannelAccountId;
    readonly externalEventId: string;
    readonly eventType: 'message.received' | 'action.triggered' | 'delivery.updated' | 'binding.requested';
    readonly payload: JsonValue;
    readonly status: 'received' | 'processing' | 'processed' | 'failed';
    readonly occurredAt: IsoDateTime;
    readonly receivedAt: IsoDateTime;
}

export type DeliveryStatus =
    'pending' | 'sending' | 'accepted' | 'delivered' | 'retryable_failed' | 'permanent_failed' | 'dead_letter';

export interface Delivery {
    readonly id: DeliveryId;
    readonly businessEventId: EventId;
    readonly correlationId: CorrelationId;
    readonly bindingId: BindingId;
    readonly channelAccountId: ChannelAccountId;
    readonly kind: 'reminder_due' | 'schedule_receipt';
    readonly semanticPayload: JsonValue;
    readonly presentationType: PresentationType;
    readonly status: DeliveryStatus;
    readonly externalMessageId?: string;
    readonly expiresAt?: IsoDateTime;
    readonly lastErrorCode?: string;
    readonly createdAt: IsoDateTime;
    readonly updatedAt: IsoDateTime;
}

/** Request-level idempotency record, including zero-delivery submissions. */
export interface IntentSubmissionRecord {
    readonly businessEventId: EventId;
    readonly kind: Delivery['kind'];
    readonly requestFingerprint: string;
    readonly submission: NotificationSubmission;
    readonly createdAt: IsoDateTime;
}

export interface DeliveryAttempt {
    readonly id: DeliveryAttemptId;
    readonly deliveryId: DeliveryId;
    readonly attemptNo: number;
    readonly requestId: RequestId;
    readonly renderedPayload: JsonValue;
    readonly status: 'sending' | 'accepted' | 'retryable_failed' | 'permanent_failed';
    readonly platformMessageId?: string;
    readonly errorCode?: string;
    readonly startedAt: IsoDateTime;
    readonly completedAt?: IsoDateTime;
}

export interface DeliveryReceipt {
    readonly id: DeliveryReceiptId;
    readonly deliveryId: DeliveryId;
    readonly attemptId?: DeliveryAttemptId;
    readonly stage: 'delivered' | 'failed';
    readonly dedupeKey: string;
    readonly externalEventId?: string;
    readonly detail?: JsonValue;
    readonly occurredAt: IsoDateTime;
    readonly receivedAt: IsoDateTime;
}

export type ActionStatus = 'pending' | 'dispatched' | 'processing' | 'succeeded' | 'failed' | 'expired';

export interface ImAction {
    readonly id: ActionId;
    readonly operationId: OperationId;
    readonly correlationId: CorrelationId;
    readonly deliveryId: DeliveryId;
    readonly actorBindingId: BindingId;
    readonly deviceId: DeviceId;
    readonly reminderTriggerId: ReminderTriggerId;
    readonly actionType: ReminderActionKind;
    readonly actionParams?: JsonValue;
    readonly actionKeyHash: string;
    readonly expectedIdentityId: ExternalIdentityId;
    readonly actualIdentityId?: ExternalIdentityId;
    readonly status: ActionStatus;
    readonly dispatchedAt?: IsoDateTime;
    readonly result?: ReminderActionResult;
    readonly expiresAt: IsoDateTime;
    readonly createdAt: IsoDateTime;
    readonly updatedAt: IsoDateTime;
}

/** Server-side transactional outbox; this is not a device Local Outbox. */
export interface ImOutboxEvent {
    readonly id: OutboxEventId;
    readonly eventType: string;
    readonly aggregateId: string;
    readonly payload: JsonValue;
    readonly status: 'pending' | 'published' | 'failed';
    readonly attempts: number;
    readonly availableAt: IsoDateTime;
    readonly createdAt: IsoDateTime;
    readonly publishedAt?: IsoDateTime;
}
