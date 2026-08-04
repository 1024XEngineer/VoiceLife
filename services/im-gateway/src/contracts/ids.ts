import type { Brand } from '../shared/types.js';

export type EventId = Brand<string, 'EventId'>;
export type CorrelationId = Brand<string, 'CorrelationId'>;
export type RequestId = Brand<string, 'RequestId'>;
export type OperationId = Brand<string, 'OperationId'>;
export type UserId = Brand<string, 'UserId'>;
export type DeviceId = Brand<string, 'DeviceId'>;

/** Cross-process IDs are opaque strings; storage adapters may map internal keys. */
export type ScheduleId = Brand<string, 'ScheduleId'>;
export type TimerTaskId = Brand<string, 'TimerTaskId'>;
export type TimerInstanceId = Brand<string, 'TimerInstanceId'>;
export type ReminderTriggerId = Brand<string, 'ReminderTriggerId'>;

export type ChannelAccountId = Brand<string, 'ChannelAccountId'>;
export type PairingSessionId = Brand<string, 'PairingSessionId'>;
export type ExternalIdentityId = Brand<string, 'ExternalIdentityId'>;
export type BindingId = Brand<string, 'BindingId'>;
export type InboundEventId = Brand<string, 'InboundEventId'>;
export type DeliveryId = Brand<string, 'DeliveryId'>;
export type DeliveryAttemptId = Brand<string, 'DeliveryAttemptId'>;
export type DeliveryReceiptId = Brand<string, 'DeliveryReceiptId'>;
export type ActionId = Brand<string, 'ActionId'>;
export type OutboxEventId = Brand<string, 'OutboxEventId'>;

export function unsafeId<T>(value: string): T {
    return value as T;
}
