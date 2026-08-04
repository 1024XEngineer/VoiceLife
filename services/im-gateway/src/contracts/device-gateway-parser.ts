import {
    DEVICE_CONTRACT_VERSION,
    type NotificationActionOption,
    type NotificationIntent,
    type ReminderActionIntent,
    type ReminderActionExecutionStatus,
    type ReminderActionResult,
    type ScheduleOperationType,
    type ScheduleReceiptIntent,
} from './device-gateway.js';
import type {
    CorrelationId,
    DeviceId,
    EventId,
    OperationId,
    ReminderTriggerId,
    ScheduleId,
    TimerInstanceId,
    TimerTaskId,
    UserId,
} from './ids.js';
import { unsafeId } from './ids.js';
import { ImGatewayError } from '../shared/errors.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';

type JsonObject = Record<string, unknown>;

const ISO_8601 = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?(?:Z|[+-](\d{2}):(\d{2}))$/;

export function parseScheduleReceiptIntent(input: unknown): ScheduleReceiptIntent {
    const value = objectAt(input, 'body');
    const userId = optionalId<UserId>(value, 'userId', 'body.userId');
    return {
        schemaVersion: contractVersion(value),
        eventId: requiredId<EventId>(value, 'eventId', 'body.eventId'),
        correlationId: requiredId<CorrelationId>(value, 'correlationId', 'body.correlationId'),
        ...(userId === undefined ? {} : { userId }),
        deviceId: requiredId<DeviceId>(value, 'deviceId', 'body.deviceId'),
        operationType: enumAt(
            value.operationType,
            ['created', 'updated', 'cancelled', 'undone'] as const,
            'body.operationType',
        ) satisfies ScheduleOperationType,
        scheduleId: requiredId<ScheduleId>(value, 'scheduleId', 'body.scheduleId'),
        result: enumAt(value.result, ['succeeded', 'failed'] as const, 'body.result'),
        summary: stringAt(value.summary, 'body.summary'),
        occurredAt: isoDateTimeAt(value.occurredAt, 'body.occurredAt'),
    };
}

export function parseNotificationIntent(input: unknown): NotificationIntent {
    const value = objectAt(input, 'body');
    const recipient = objectAt(value.recipient, 'body.recipient');
    const content = objectAt(value.content, 'body.content');
    const contentBody = optionalString(content, 'body', 'body.content.body');
    const reminderType = enumAt(value.reminderType, ['weak', 'strong'] as const, 'body.reminderType');
    if (!Array.isArray(value.actions)) {
        invalid('body.actions', 'must be an array');
    }
    const actions = value.actions.map((action, index) => parseNotificationAction(action, `body.actions[${index}]`));
    if (reminderType === 'weak' && actions.length !== 0) {
        invalid('body.actions', 'must be empty for a weak reminder');
    }
    if (reminderType === 'strong' && actions.length === 0) {
        invalid('body.actions', 'must contain at least one action for a strong reminder');
    }

    const base = {
        schemaVersion: contractVersion(value),
        businessEventId: requiredId<EventId>(value, 'businessEventId', 'body.businessEventId'),
        correlationId: requiredId<CorrelationId>(value, 'correlationId', 'body.correlationId'),
        kind: enumAt(value.kind, ['reminder_due'] as const, 'body.kind'),
        recipient: {
            userId: requiredId<UserId>(recipient, 'userId', 'body.recipient.userId'),
            deviceId: requiredId<DeviceId>(recipient, 'deviceId', 'body.recipient.deviceId'),
        },
        scheduleId: requiredId<ScheduleId>(value, 'scheduleId', 'body.scheduleId'),
        taskId: requiredId<TimerTaskId>(value, 'taskId', 'body.taskId'),
        instanceId: requiredId<TimerInstanceId>(value, 'instanceId', 'body.instanceId'),
        reminderTriggerId: requiredId<ReminderTriggerId>(value, 'reminderTriggerId', 'body.reminderTriggerId'),
        content: {
            title: stringAt(content.title, 'body.content.title'),
            ...(contentBody === undefined ? {} : { body: contentBody }),
        },
        plannedAt: isoDateTimeAt(value.plannedAt, 'body.plannedAt'),
        triggerAt: isoDateTimeAt(value.triggerAt, 'body.triggerAt'),
        occurredAt: isoDateTimeAt(value.occurredAt, 'body.occurredAt'),
    };

    return reminderType === 'weak'
        ? { ...base, reminderType, actions: [] }
        : {
              ...base,
              reminderType,
              actions: actions as [NotificationActionOption, ...NotificationActionOption[]],
          };
}

export function parseReminderActionResult(input: unknown): ReminderActionResult {
    const value = objectAt(input, 'body');
    const nextTriggerAt = optionalIsoDateTime(value, 'nextTriggerAt', 'body.nextTriggerAt');
    const errorCode = optionalString(value, 'errorCode', 'body.errorCode');
    const details = optionalJsonValue(value, 'details', 'body.details');
    return {
        schemaVersion: contractVersion(value),
        operationId: requiredId<OperationId>(value, 'operationId', 'body.operationId'),
        reminderTriggerId: requiredId<ReminderTriggerId>(value, 'reminderTriggerId', 'body.reminderTriggerId'),
        status: enumAt(
            value.status,
            ['succeeded', 'retryable_failed', 'failed', 'expired'] as const,
            'body.status',
        ) satisfies ReminderActionExecutionStatus,
        ...(nextTriggerAt === undefined ? {} : { nextTriggerAt }),
        ...(errorCode === undefined ? {} : { errorCode }),
        ...(details === undefined ? {} : { details }),
        occurredAt: isoDateTimeAt(value.occurredAt, 'body.occurredAt'),
    };
}

export function parseReminderActionIntent(input: unknown): ReminderActionIntent {
    const value = objectAt(input, 'body');
    const action = enumAt(value.action, ['acknowledge', 'snooze'] as const, 'body.action');
    const params = value.params === undefined ? undefined : parseActionParams(value.params, 'body.params');
    if (action === 'snooze' && params === undefined) {
        invalid('body.params', 'is required for a snooze action');
    }
    if (action === 'acknowledge' && params !== undefined) {
        invalid('body.params', 'must be omitted for an acknowledge action');
    }
    return {
        token: stringAt(value.token, 'body.token'),
        action,
        ...(params === undefined ? {} : { params }),
    };
}

export function parseActionToken(input: unknown): string {
    return stringAt(input, 'token');
}

function parseNotificationAction(input: unknown, path: string): NotificationActionOption {
    const value = objectAt(input, path);
    const type = enumAt(value.type, ['acknowledge', 'snooze'] as const, `${path}.type`);
    const params = value.params === undefined ? undefined : parseActionParams(value.params, `${path}.params`);
    if (type === 'snooze' && params === undefined) {
        invalid(`${path}.params`, 'is required for a snooze action');
    }
    return {
        kind: enumAt(value.kind, ['command'] as const, `${path}.kind`),
        type,
        label: stringAt(value.label, `${path}.label`),
        ...(params === undefined ? {} : { params }),
    };
}

function parseActionParams(input: unknown, path: string): { readonly minutes: number } {
    const value = objectAt(input, path);
    if (typeof value.minutes !== 'number' || !Number.isInteger(value.minutes) || value.minutes <= 0) {
        invalid(`${path}.minutes`, 'must be a positive integer');
    }
    return { minutes: value.minutes };
}

function contractVersion(value: JsonObject): typeof DEVICE_CONTRACT_VERSION {
    if (value.schemaVersion !== DEVICE_CONTRACT_VERSION) {
        invalid('body.schemaVersion', `must equal ${DEVICE_CONTRACT_VERSION}`);
    }
    return DEVICE_CONTRACT_VERSION;
}

function objectAt(input: unknown, path: string): JsonObject {
    if (typeof input !== 'object' || input === null || Array.isArray(input)) {
        invalid(path, 'must be an object');
    }
    return input as JsonObject;
}

function stringAt(input: unknown, path: string): string {
    if (typeof input !== 'string' || input.length === 0) {
        invalid(path, 'must be a non-empty string');
    }
    return input;
}

function requiredId<T>(value: JsonObject, key: string, path: string): T {
    return unsafeId<T>(stringAt(value[key], path));
}

function optionalId<T>(value: JsonObject, key: string, path: string): T | undefined {
    return value[key] === undefined ? undefined : requiredId<T>(value, key, path);
}

function optionalString(value: JsonObject, key: string, path: string): string | undefined {
    return value[key] === undefined ? undefined : stringAt(value[key], path);
}

function enumAt<const T extends readonly string[]>(input: unknown, allowed: T, path: string): T[number] {
    if (typeof input !== 'string' || !allowed.includes(input)) {
        invalid(path, `must be one of: ${allowed.join(', ')}`);
    }
    return input as T[number];
}

function isoDateTimeAt(input: unknown, path: string): IsoDateTime {
    if (typeof input !== 'string' || !isValidIsoDateTime(input)) {
        invalid(path, 'must be a valid ISO 8601 date-time with timezone');
    }
    return input as IsoDateTime;
}

function isValidIsoDateTime(input: string): boolean {
    const match = ISO_8601.exec(input);
    if (match === null) return false;
    const year = Number(match[1]);
    const month = Number(match[2]);
    const day = Number(match[3]);
    const hour = Number(match[4]);
    const minute = Number(match[5]);
    const second = Number(match[6]);
    const offsetHour = match[8] === undefined ? 0 : Number(match[8]);
    const offsetMinute = match[9] === undefined ? 0 : Number(match[9]);
    const leapYear = year % 4 === 0 && (year % 100 !== 0 || year % 400 === 0);
    const daysInMonth = [31, leapYear ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    return (
        month >= 1 &&
        month <= 12 &&
        day >= 1 &&
        day <= (daysInMonth[month - 1] ?? 0) &&
        hour <= 23 &&
        minute <= 59 &&
        second <= 59 &&
        offsetHour <= 23 &&
        offsetMinute <= 59 &&
        Number.isFinite(Date.parse(input))
    );
}

function optionalIsoDateTime(value: JsonObject, key: string, path: string): IsoDateTime | undefined {
    return value[key] === undefined ? undefined : isoDateTimeAt(value[key], path);
}

function optionalJsonValue(value: JsonObject, key: string, path: string): JsonValue | undefined {
    return value[key] === undefined ? undefined : jsonValueAt(value[key], path);
}

function jsonValueAt(input: unknown, path: string): JsonValue {
    if (input === null || typeof input === 'string' || typeof input === 'boolean') {
        return input;
    }
    if (typeof input === 'number' && Number.isFinite(input)) return input;
    if (Array.isArray(input)) {
        return input.map((item, index) => jsonValueAt(item, `${path}[${index}]`));
    }
    if (typeof input === 'object') {
        return Object.fromEntries(
            Object.entries(input).map(([key, item]) => [key, jsonValueAt(item, `${path}.${key}`)]),
        );
    }
    return invalid(path, 'must be a JSON value');
}

function invalid(path: string, expectation: string): never {
    throw new ImGatewayError('invalid_contract', `Invalid device contract at ${path}: ${expectation}`);
}
