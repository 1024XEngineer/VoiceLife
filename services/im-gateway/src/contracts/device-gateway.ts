import type {
    ActionId,
    BindingId,
    CorrelationId,
    DeliveryId,
    DeviceId,
    EventId,
    OperationId,
    ReminderTriggerId,
    ScheduleId,
    TimerInstanceId,
    TimerTaskId,
    UserId,
} from './ids.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';

export const DEVICE_CONTRACT_VERSION = '1' as const;

export type ScheduleOperationType = 'created' | 'updated' | 'cancelled' | 'undone';

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

export type ReminderType = 'weak' | 'strong';
export type ReminderActionKind = 'acknowledge' | 'snooze';
export type ActionIntentKind = ReminderActionKind | 'bind_confirm' | 'bind_cancel' | 'open_url';

/** Platform-independent shape shared by H5 and native action entry points. */
export interface ActionIntent {
    readonly token: string;
    readonly action: ActionIntentKind;
    readonly params?: JsonValue;
}

export type ReminderActionIntent = Pick<ActionIntent, 'token' | 'params'> & {
    readonly action: ReminderActionKind;
};

export interface NotificationActionOption {
    readonly kind: 'command';
    readonly type: ReminderActionKind;
    readonly label: string;
    readonly params?: { readonly minutes: number };
}

export interface NotificationRecipient {
    readonly userId: UserId;
    readonly deviceId: DeviceId;
}

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

export type ReminderActionExecutionStatus = 'succeeded' | 'retryable_failed' | 'failed' | 'expired';

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
