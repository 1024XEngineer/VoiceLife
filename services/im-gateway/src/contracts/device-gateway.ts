import type {
    ActionId,
    BindingId,
    CorrelationId,
    DeliveryId,
    DeviceId,
    EventId,
    OperationId,
    PairingSessionId,
    ReminderTriggerId,
    ScheduleId,
    TimerInstanceId,
    TimerTaskId,
    UserId,
} from './ids.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';
import type { ImPlatform } from './platform-events.js';

/** 当前设备与 Gateway 之间的契约版本。 */
export const DEVICE_CONTRACT_VERSION = '1' as const;

/** 配对会话允许的最短有效期（分钟）。 */
export const MIN_PAIRING_SESSION_MINUTES = 1;
/** 配对会话允许的最长有效期（分钟）。 */
export const MAX_PAIRING_SESSION_MINUTES = 10;

/** 设备创建配对会话的请求契约。 */
export interface CreatePairingSessionRequest {
    readonly userId?: UserId;
    readonly deviceId: DeviceId;
    readonly allowedPlatforms?: readonly ImPlatform[];
    readonly expiresInMinutes?: number;
}

/** 设备 HTTP 边界可见的配对状态；不包含内部展示码 hash 或外部身份。 */
export interface PairingSessionStatus {
    readonly id: PairingSessionId;
    readonly userId?: UserId;
    readonly deviceId: DeviceId;
    readonly allowedPlatforms?: readonly ImPlatform[];
    readonly status: 'pending' | 'confirmed' | 'expired' | 'cancelled';
    readonly expiresAt: IsoDateTime;
    readonly createdAt: IsoDateTime;
    readonly confirmedAt?: IsoDateTime;
}

/** 设备创建配对会话后收到的公开状态与一次性六位码。 */
export interface CreatedPairingSessionResponse {
    readonly session: PairingSessionStatus;
    readonly displayCode: string;
}

/** 日程变更产生回执时的操作类型。 */
export type ScheduleOperationType = 'created' | 'updated' | 'cancelled' | 'undone';

/** 设备上报日程操作结果的契约。 */
export interface ScheduleReceiptIntent {
    readonly schemaVersion: typeof DEVICE_CONTRACT_VERSION;
    readonly eventId: EventId;
    readonly correlationId: CorrelationId;
    readonly userId?: UserId;
    readonly deviceId: DeviceId;
    readonly operationType: ScheduleOperationType;
    readonly scheduleId: ScheduleId;
    readonly result: 'succeeded' | 'failed';
    readonly summary: string;
    readonly occurredAt: IsoDateTime;
}

/** 语音日程查询通过 IM 返回的完整结果。 */
export interface ScheduleQueryResultIntent {
    readonly schemaVersion: typeof DEVICE_CONTRACT_VERSION;
    readonly businessEventId: EventId;
    readonly correlationId: CorrelationId;
    readonly userId?: UserId;
    readonly deviceId: DeviceId;
    readonly query: {
        readonly keyword?: string;
        readonly status: 'all' | 'active' | 'cancelled' | 'completed';
        readonly startDate?: string;
        readonly endDate?: string;
    };
    readonly resultCount: number;
    readonly schedules: readonly JsonValue[];
    readonly futureOccurrences: readonly JsonValue[];
    readonly exceptions: readonly JsonValue[];
    readonly queriedAt: IsoDateTime;
}

/** 提醒的强弱等级。 */
export type ReminderType = 'weak' | 'strong';
/** 设备能够执行的提醒动作。 */
export type ReminderActionKind = 'acknowledge' | 'snooze';
/** 设备语音直接消费后的跨入口状态事实。 */
export interface VoiceReminderActionStatus {
    readonly schemaVersion: typeof DEVICE_CONTRACT_VERSION;
    readonly eventId: EventId;
    readonly correlationId: CorrelationId;
    readonly deviceId: DeviceId;
    readonly reminderTriggerId: ReminderTriggerId;
    readonly operationId: OperationId;
    readonly action: ReminderActionKind;
    readonly status: 'succeeded' | 'failed';
    readonly occurredAt: IsoDateTime;
    readonly nextTriggerAt?: IsoDateTime;
    readonly source: 'voice';
}
/** 所有可由通知入口触发的动作类型。 */
export type ActionIntentKind = ReminderActionKind | 'bind_confirm' | 'bind_cancel' | 'open_url';

/** H5 与平台原生动作入口共享的平台无关载荷。 */
export interface ActionIntent {
    readonly token: string;
    readonly action: ActionIntentKind;
    readonly params?: JsonValue;
}

/** 已收窄为提醒操作的动作入口载荷。 */
export type ReminderActionIntent = Pick<ActionIntent, 'token' | 'params'> & {
    readonly action: ReminderActionKind;
};

/** 强提醒中可呈现给用户的操作选项。 */
export interface NotificationActionOption {
    readonly kind: 'command';
    readonly type: ReminderActionKind;
    readonly label: string;
    readonly params?: { readonly minutes: number };
}

/** 通知目标对应的 VoiceLife 用户与设备。 */
export interface NotificationRecipient {
    readonly userId: UserId;
    readonly deviceId: DeviceId;
}

/** 跨平台通知的语义内容。 */
export interface NotificationContent {
    readonly title: string;
    readonly body?: string;
}

interface NotificationIntentBase {
    readonly schemaVersion: typeof DEVICE_CONTRACT_VERSION;
    readonly businessEventId: EventId;
    readonly correlationId: CorrelationId;
    readonly kind: 'reminder_due';
    readonly scheduleId: ScheduleId;
    readonly taskId: TimerTaskId;
    readonly instanceId: TimerInstanceId;
    readonly reminderTriggerId: ReminderTriggerId;
    readonly content: NotificationContent;
    readonly plannedAt: IsoDateTime;
    readonly triggerAt: IsoDateTime;
    readonly occurredAt: IsoDateTime;
}

/** 按提醒强弱约束收件人与动作的通知意图。 */
export type NotificationIntent = NotificationIntentBase &
    (
        | {
              readonly recipient: NotificationRecipient;
              readonly reminderType: 'weak';
              readonly actions: readonly [];
          }
        | {
              readonly recipient: NotificationRecipient;
              readonly reminderType: 'strong';
              readonly actions: readonly [NotificationActionOption, ...NotificationActionOption[]];
          }
    );

/** Gateway 接收通知意图后的投递受理结果。 */
export interface NotificationSubmission {
    readonly businessEventId: EventId;
    readonly status: 'accepted';
    readonly deliveries: readonly {
        readonly deliveryId: DeliveryId;
        readonly bindingId: BindingId;
        readonly status: 'pending';
    }[];
    readonly actionStream?: {
        readonly reminderTriggerId: ReminderTriggerId;
        readonly expiresAt: IsoDateTime;
    };
}

/** Gateway 下发给设备执行的提醒动作命令。 */
export interface ReminderActionCommand {
    readonly schemaVersion: typeof DEVICE_CONTRACT_VERSION;
    readonly commandId: ActionId;
    readonly operationId: OperationId;
    readonly correlationId: CorrelationId;
    readonly deviceId: DeviceId;
    readonly actorBindingId: BindingId;
    readonly reminderTriggerId: ReminderTriggerId;
    readonly action: ReminderActionKind;
    readonly params?: { readonly minutes: number };
    readonly occurredAt: IsoDateTime;
    readonly expiresAt: IsoDateTime;
}

/** 设备执行提醒动作后的终态。 */
export type ReminderActionExecutionStatus = 'succeeded' | 'retryable_failed' | 'failed' | 'expired';

/** 设备回传的提醒动作执行结果。 */
export interface ReminderActionResult {
    readonly schemaVersion: typeof DEVICE_CONTRACT_VERSION;
    readonly operationId: OperationId;
    readonly reminderTriggerId: ReminderTriggerId;
    readonly status: ReminderActionExecutionStatus;
    readonly nextTriggerAt?: IsoDateTime;
    readonly errorCode?: string;
    readonly details?: JsonValue;
    readonly occurredAt: IsoDateTime;
}
