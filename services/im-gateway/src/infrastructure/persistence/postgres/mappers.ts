import type {
    NotificationSubmission,
    ReminderActionKind,
    ReminderActionResult,
    ReminderActionStatusReport,
} from '../../../contracts/device-gateway.js';
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
} from '../../../contracts/ids.js';
import type { ImPlatform } from '../../../contracts/platform-events.js';
import type {
    ActionStatus,
    ChannelAccount,
    Delivery,
    DeliveryAttempt,
    DeliveryReceipt,
    DeviceReminderActionFact,
    DeliveryStatus,
    ExternalIdentity,
    ImAction,
    ImBinding,
    ImDevice,
    ImOutboxEvent,
    InboundEventRecord,
    IntentSubmissionRecord,
    PairingSession,
    PresentationType,
} from '../../../domain/models.js';
import type { IsoDateTime, JsonValue } from '../../../shared/types.js';

/** PostgreSQL 返回的单行，统一按未知值读取。 */
export type DbRow = { readonly [column: string]: unknown };

/**
 * 将 pg 返回的时间值规范化为 ISO 8601 UTC 字符串。
 * @param value pg 返回的时间值，可能是 Date 或已字符串化的 ISO 时间。
 * @returns 规范化的 ISO 8601 UTC 字符串。
 */
export function toIso(value: unknown): IsoDateTime {
    return (value instanceof Date ? value.toISOString() : value) as IsoDateTime;
}

/**
 * 将渠道账号行映射为领域模型。
 * @param row PostgreSQL 渠道账号表行。
 * @returns 渠道账号领域模型。
 */
export function mapChannelAccount(row: DbRow): ChannelAccount {
    return {
        id: row.id as ChannelAccountId,
        platform: row.platform as ImPlatform,
        tenantExternalId: row.tenant_external_id as string,
        koishiBotId: row.koishi_bot_id as string,
        credentialRef: row.credential_ref as string,
        connectionMode: row.connection_mode as ChannelAccount['connectionMode'],
        ...(row.capability_config === null ? {} : { capabilityConfig: row.capability_config as JsonValue }),
        status: row.status as ChannelAccount['status'],
        createdAt: toIso(row.created_at),
        updatedAt: toIso(row.updated_at),
    };
}

/**
 * 将设备行映射为领域模型。
 * @param row PostgreSQL 设备表行。
 * @returns 设备领域模型。
 */
export function mapDevice(row: DbRow): ImDevice {
    return {
        deviceId: row.device_id as DeviceId,
        userId: row.user_id as UserId,
        tokenDigest: row.token_digest as Uint8Array,
        status: row.status as ImDevice['status'],
        createdAt: toIso(row.created_at),
        updatedAt: toIso(row.updated_at),
    };
}

/**
 * 将配对会话行映射为领域模型。
 * @param row PostgreSQL 配对会话表行。
 * @returns 配对会话领域模型。
 */
export function mapPairingSession(row: DbRow): PairingSession {
    return {
        id: row.id as PairingSessionId,
        displayCodeHash: row.display_code_hash as string,
        ...(row.user_id === null ? {} : { userId: row.user_id as UserId }),
        deviceId: row.device_id as DeviceId,
        ...(row.allowed_platforms === null ? {} : { allowedPlatforms: row.allowed_platforms as readonly ImPlatform[] }),
        status: row.status as PairingSession['status'],
        expiresAt: toIso(row.expires_at),
        createdAt: toIso(row.created_at),
        ...(row.confirmed_at === null ? {} : { confirmedAt: toIso(row.confirmed_at) }),
    };
}

/**
 * 将外部身份行映射为领域模型。
 * @param row PostgreSQL 外部身份表行。
 * @returns 外部身份领域模型。
 */
export function mapExternalIdentity(row: DbRow): ExternalIdentity {
    return {
        id: row.id as ExternalIdentityId,
        channelAccountId: row.channel_account_id as ChannelAccountId,
        externalUserIdCiphertext: row.external_user_id_ciphertext as string,
        externalUserIdHash: row.external_user_id_hash as string,
        ...(row.display_name === null ? {} : { displayName: row.display_name as string }),
        status: row.status as ExternalIdentity['status'],
        createdAt: toIso(row.created_at),
        updatedAt: toIso(row.updated_at),
    };
}

/**
 * 将绑定行映射为领域模型。
 * @param row PostgreSQL 绑定表行。
 * @returns 绑定领域模型。
 */
export function mapBinding(row: DbRow): ImBinding {
    return {
        id: row.id as BindingId,
        userId: row.user_id as UserId,
        ...(row.device_id === null ? {} : { deviceId: row.device_id as DeviceId }),
        externalIdentityId: row.external_identity_id as ExternalIdentityId,
        priority: row.priority as number,
        status: row.status as ImBinding['status'],
        boundAt: toIso(row.bound_at),
        ...(row.unbound_at === null ? {} : { unboundAt: toIso(row.unbound_at) }),
        ...(row.revoked_at === null ? {} : { revokedAt: toIso(row.revoked_at) }),
    };
}

/**
 * 将入站事件行映射为领域模型。
 * @param row PostgreSQL 入站事件表行。
 * @returns 入站事件领域模型。
 */
export function mapInboundEvent(row: DbRow): InboundEventRecord {
    return {
        id: row.id as InboundEventId,
        channelAccountId: row.channel_account_id as ChannelAccountId,
        externalEventId: row.external_event_id as string,
        eventType: row.event_type as InboundEventRecord['eventType'],
        payload: row.payload as JsonValue,
        status: row.status as InboundEventRecord['status'],
        occurredAt: toIso(row.occurred_at),
        receivedAt: toIso(row.received_at),
    };
}

/**
 * 将受理记录行映射为领域模型。
 * @param row PostgreSQL 受理记录表行。
 * @returns 受理记录领域模型。
 */
export function mapIntentSubmission(row: DbRow): IntentSubmissionRecord {
    return {
        businessEventId: row.business_event_id as EventId,
        kind: row.kind as IntentSubmissionRecord['kind'],
        requestFingerprint: row.request_fingerprint as string,
        submission: row.submission as NotificationSubmission,
        createdAt: toIso(row.created_at),
    };
}

/**
 * 将投递行映射为领域模型。
 * @param row PostgreSQL 投递表行。
 * @returns 投递领域模型。
 */
export function mapDelivery(row: DbRow): Delivery {
    return {
        id: row.id as DeliveryId,
        businessEventId: row.business_event_id as EventId,
        correlationId: row.correlation_id as CorrelationId,
        bindingId: row.binding_id as BindingId,
        channelAccountId: row.channel_account_id as ChannelAccountId,
        kind: row.kind as Delivery['kind'],
        semanticPayload: row.semantic_payload as JsonValue,
        presentationType: row.presentation_type as PresentationType,
        status: row.status as DeliveryStatus,
        ...(row.external_message_id === null ? {} : { externalMessageId: row.external_message_id as string }),
        ...(row.expires_at === null ? {} : { expiresAt: toIso(row.expires_at) }),
        ...(row.last_error_code === null ? {} : { lastErrorCode: row.last_error_code as string }),
        ...(row.claimed_at === null ? {} : { claimedAt: toIso(row.claimed_at) }),
        ...(row.claim_token === null ? {} : { claimToken: row.claim_token as string }),
        createdAt: toIso(row.created_at),
        updatedAt: toIso(row.updated_at),
    };
}

/**
 * 将发送尝试行映射为领域模型。
 * @param row PostgreSQL 发送尝试表行。
 * @returns 发送尝试领域模型。
 */
export function mapDeliveryAttempt(row: DbRow): DeliveryAttempt {
    return {
        id: row.id as DeliveryAttemptId,
        deliveryId: row.delivery_id as DeliveryId,
        attemptNo: row.attempt_no as number,
        requestId: row.request_id as RequestId,
        renderedPayload: row.rendered_payload as JsonValue,
        status: row.status as DeliveryAttempt['status'],
        ...(row.platform_message_id === null ? {} : { platformMessageId: row.platform_message_id as string }),
        ...(row.error_code === null ? {} : { errorCode: row.error_code as string }),
        startedAt: toIso(row.started_at),
        ...(row.completed_at === null ? {} : { completedAt: toIso(row.completed_at) }),
    };
}

/**
 * 将投递回执行映射为领域模型。
 * @param row PostgreSQL 投递回执表行。
 * @returns 投递回执领域模型。
 */
export function mapDeliveryReceipt(row: DbRow): DeliveryReceipt {
    return {
        id: row.id as DeliveryReceiptId,
        deliveryId: row.delivery_id as DeliveryId,
        ...(row.attempt_id === null ? {} : { attemptId: row.attempt_id as DeliveryAttemptId }),
        stage: row.stage as DeliveryReceipt['stage'],
        dedupeKey: row.dedupe_key as string,
        ...(row.external_event_id === null ? {} : { externalEventId: row.external_event_id as string }),
        ...(row.detail === null ? {} : { detail: row.detail as JsonValue }),
        occurredAt: toIso(row.occurred_at),
        receivedAt: toIso(row.received_at),
    };
}

/**
 * 将动作行映射为领域模型。
 * @param row PostgreSQL 动作表行。
 * @returns 提醒动作领域模型。
 */
export function mapAction(row: DbRow): ImAction {
    return {
        id: row.id as ActionId,
        operationId: row.operation_id as OperationId,
        correlationId: row.correlation_id as CorrelationId,
        deliveryId: row.delivery_id as DeliveryId,
        actorBindingId: row.actor_binding_id as BindingId,
        deviceId: row.device_id as DeviceId,
        reminderTriggerId: row.reminder_trigger_id as ReminderTriggerId,
        actionType: row.action_type as ReminderActionKind,
        ...(row.action_params === null ? {} : { actionParams: row.action_params as JsonValue }),
        actionKeyHash: row.action_key_hash as string,
        expectedIdentityId: row.expected_identity_id as ExternalIdentityId,
        ...(row.actual_identity_id === null ? {} : { actualIdentityId: row.actual_identity_id as ExternalIdentityId }),
        status: row.status as ActionStatus,
        ...(row.dispatched_at === null ? {} : { dispatchedAt: toIso(row.dispatched_at) }),
        ...(row.result === null ? {} : { result: row.result as ReminderActionResult }),
        expiresAt: toIso(row.expires_at),
        createdAt: toIso(row.created_at),
        updatedAt: toIso(row.updated_at),
    };
}

/**
 * 将设备语音动作事实行映射为领域记录。
 * @param row PostgreSQL 查询返回的动作事实行。
 * @returns 可供应用层使用的设备动作事实。
 */
export function mapReminderActionFact(row: DbRow): DeviceReminderActionFact {
    return {
        eventId: row.event_id as EventId,
        fingerprint: row.fingerprint as string,
        report: {
            schemaVersion: row.schema_version as '1',
            eventId: row.event_id as EventId,
            correlationId: row.correlation_id as CorrelationId,
            deviceId: row.device_id as DeviceId,
            reminderTriggerId: row.reminder_trigger_id as ReminderTriggerId,
            operationId: row.operation_id as OperationId,
            action: row.action as ReminderActionKind,
            status: row.status as ReminderActionStatusReport['status'],
            occurredAt: toIso(row.occurred_at),
            ...(row.next_trigger_at === null ? {} : { nextTriggerAt: toIso(row.next_trigger_at) }),
            ...(row.error_code === null ? {} : { errorCode: row.error_code as string }),
            ...(row.details === null ? {} : { details: row.details as JsonValue }),
            source: 'voice',
        },
        receivedAt: toIso(row.received_at),
    };
}

/**
 * 将发件箱事件行映射为领域模型。
 * @param row PostgreSQL 发件箱事件表行。
 * @returns 发件箱事件领域模型。
 */
export function mapOutboxEvent(row: DbRow): ImOutboxEvent {
    return {
        id: row.id as OutboxEventId,
        eventType: row.event_type as string,
        aggregateId: row.aggregate_id as string,
        payload: row.payload as JsonValue,
        status: row.status as ImOutboxEvent['status'],
        attempts: row.attempts as number,
        availableAt: toIso(row.available_at),
        createdAt: toIso(row.created_at),
        ...(row.published_at === null ? {} : { publishedAt: toIso(row.published_at) }),
    };
}
