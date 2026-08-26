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
import type {
    NotificationSubmission,
    ReminderActionKind,
    ReminderActionResult,
    ReminderActionStatusReport,
} from '../contracts/device-gateway.js';
import type { ImPlatform } from '../contracts/platform-events.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';

/** 渠道账号可用的消息与交互能力。 */
export interface ChannelCapabilities {
    readonly proactiveMessage: boolean;
    readonly nativeAction: boolean;
    readonly actionUi: boolean;
    readonly deliveryReceipt: boolean;
    readonly presentationTypes: readonly PresentationType[];
}

/** 渠道可接受的消息呈现形式。 */
export type PresentationType = 'native_card' | 'template' | 'rich_text' | 'text_with_action_ui';

/** Gateway 管理的租户级 IM 渠道账号。 */
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

/** Gateway 注册且可独立吊销/轮换凭据的物理设备。 */
export interface ImDevice {
    readonly deviceId: DeviceId;
    readonly userId: UserId;
    /** SHA-256 原始 32 字节摘要；永不持久化或返回明文 Token。 */
    readonly tokenDigest: Uint8Array;
    readonly status: 'active' | 'revoked';
    readonly createdAt: IsoDateTime;
    readonly updatedAt: IsoDateTime;
}

/** 设备与外部 IM 身份建立绑定前的短期会话。 */
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

/** 渠道账号下经过保护的外部用户身份。 */
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

/** 向外部身份发送消息时使用的会话引用。 */
export interface ConversationRef {
    readonly channelAccountId: ChannelAccountId;
    readonly externalIdentityId: ExternalIdentityId;
    readonly kind: 'direct' | 'group';
    readonly externalConversationIdCiphertext: string;
}

/** VoiceLife 用户或设备与外部 IM 身份的绑定关系。 */
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

/** 用于入站幂等和处理状态跟踪的事件记录。 */
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

/** 消息投递的处理状态。 */
export type DeliveryStatus =
    'pending' | 'sending' | 'accepted' | 'delivered' | 'retryable_failed' | 'permanent_failed' | 'dead_letter';

/** 从业务事件到外部消息的一次投递。 */
export interface Delivery {
    readonly id: DeliveryId;
    readonly businessEventId: EventId;
    readonly correlationId: CorrelationId;
    readonly bindingId: BindingId;
    readonly channelAccountId: ChannelAccountId;
    readonly kind: 'reminder_due' | 'schedule_receipt' | 'schedule_query_result';
    readonly semanticPayload: JsonValue;
    readonly presentationType: PresentationType;
    readonly status: DeliveryStatus;
    readonly externalMessageId?: string;
    readonly expiresAt?: IsoDateTime;
    readonly lastErrorCode?: string;
    /** 派发领取时间；凭 lease 过期后允许其他 worker 重领（崩溃恢复）。 */
    readonly claimedAt?: IsoDateTime;
    /** 派发所有权令牌：仅持牌 worker 的写回可生效，隔离过期 worker 的迟到覆盖。 */
    readonly claimToken?: string;
    readonly createdAt: IsoDateTime;
    readonly updatedAt: IsoDateTime;
}

/** 请求级幂等记录，也覆盖没有产生投递的受理结果。 */
export interface IntentSubmissionRecord {
    readonly businessEventId: EventId;
    readonly kind: Delivery['kind'];
    readonly requestFingerprint: string;
    readonly submission: NotificationSubmission;
    readonly createdAt: IsoDateTime;
}

/** 对同一投递执行的一次发送尝试。 */
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

/** 外部平台返回的投递终态证据。 */
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

/** 用户提醒动作的生命周期状态。 */
export type ActionStatus = 'pending' | 'dispatched' | 'processing' | 'succeeded' | 'failed' | 'expired';

/** 用户通过通知入口触发的一次提醒动作。 */
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

/** 设备本地动作事实的持久化记录；eventId 与 fingerprint 提供幂等和冲突检测。 */
export interface DeviceReminderActionFact {
    readonly eventId: EventId;
    readonly fingerprint: string;
    readonly report: ReminderActionStatusReport;
    readonly receivedAt: IsoDateTime;
}

/** 服务端事务性发件箱事件，不代表设备侧 Local Outbox。 */
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
