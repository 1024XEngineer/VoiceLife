import type { ActionId, DeviceId, PairingSessionId, ReminderTriggerId } from '../../contracts/ids.js';
import type { NotificationSubmission, ReminderActionCommand } from '../../contracts/device-gateway.js';
import {
    parseNotificationIntent,
    parseReminderActionResult,
    parseScheduleReceiptIntent,
} from '../../contracts/device-gateway-parser.js';
import type {
    ActionApplication,
    CreatePairingSessionCommand,
    CreatedPairingSession,
    NotificationApplication,
    PairingApplication,
} from '../../application/api.js';
import type { ImAction, PairingSession } from '../../domain/models.js';
import type { ActionCommandStreamPort, DeviceAuthenticationPort } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';

export const DEVICE_API_ROUTES = {
    pairingSessions: '/v1/im/pairing-sessions',
    pairingSession: '/v1/im/pairing-sessions/:pairingSessionId',
    scheduleReceipts: '/v1/im/schedule-receipts',
    notifications: '/v1/im/notifications',
    reminderActionResults: '/v1/devices/:deviceId/reminder-actions/:commandId/result',
    reminderActionStream: '/v1/devices/:deviceId/reminder-actions/stream',
} as const;

/** Normative transport mapping for the four cross-module contracts. */
export const DEVICE_API_ENDPOINTS = {
    scheduleReceipt: {
        method: 'POST',
        path: DEVICE_API_ROUTES.scheduleReceipts,
        transport: 'https',
    },
    notification: {
        method: 'POST',
        path: DEVICE_API_ROUTES.notifications,
        transport: 'https',
    },
    reminderActionCommand: {
        method: 'GET',
        path: DEVICE_API_ROUTES.reminderActionStream,
        transport: 'sse',
    },
    reminderActionResult: {
        method: 'POST',
        path: DEVICE_API_ROUTES.reminderActionResults,
        transport: 'https',
    },
} as const;

export interface AuthenticatedIntentRequest {
    readonly authorization: string;
    readonly idempotencyKey: string;
    readonly body: unknown;
}

/** Framework-neutral HTTP controller contract for the device-facing surface. */
export class DeviceIntentController {
    public constructor(
        private readonly notifications: NotificationApplication,
        private readonly actions: ActionApplication,
        private readonly authentication: DeviceAuthenticationPort,
        private readonly pairing: PairingApplication,
    ) {}

    public async postPairingSession(input: {
        readonly authorization: string;
        readonly body: CreatePairingSessionCommand;
    }): Promise<CreatedPairingSession> {
        await this.authenticateDevice(input.authorization, input.body.deviceId);
        return this.pairing.create(input.body);
    }

    public async getPairingSession(input: {
        readonly authorization: string;
        readonly pairingSessionId: PairingSessionId;
    }): Promise<PairingSession | undefined> {
        const principal = await this.authentication.authenticate(input.authorization);
        const session = await this.pairing.find(input.pairingSessionId);
        if (session === undefined || session.deviceId !== principal.deviceId) {
            return undefined;
        }
        return session;
    }

    public async postScheduleReceipt(input: AuthenticatedIntentRequest): Promise<NotificationSubmission> {
        const body = parseScheduleReceiptIntent(input.body);
        await this.authenticateDevice(input.authorization, body.deviceId);
        this.assertIdempotencyKey(input.idempotencyKey, body.eventId);
        return this.notifications.submitScheduleReceipt(body);
    }

    public async postNotification(input: AuthenticatedIntentRequest): Promise<NotificationSubmission> {
        const body = parseNotificationIntent(input.body);
        await this.authenticateDevice(input.authorization, body.recipient.deviceId);
        this.assertIdempotencyKey(input.idempotencyKey, body.businessEventId);
        return this.notifications.submitNotification(body);
    }

    public async postReminderActionResult(input: {
        readonly authorization: string;
        readonly deviceId: DeviceId;
        readonly commandId: ActionId;
        readonly body: unknown;
    }): Promise<ImAction> {
        const body = parseReminderActionResult(input.body);
        const principal = await this.authentication.authenticate(input.authorization);
        if (principal.deviceId !== input.deviceId) {
            throw new ImGatewayError('invalid_transition', 'Device principal does not match the result path');
        }
        return this.actions.recordResult(input.commandId, input.deviceId, body);
    }

    private async authenticateDevice(authorization: string, expectedDeviceId: DeviceId): Promise<void> {
        const principal = await this.authentication.authenticate(authorization);
        if (principal.deviceId !== expectedDeviceId) {
            throw new ImGatewayError('invalid_transition', 'Device principal does not match the intent body');
        }
    }

    private assertIdempotencyKey(idempotencyKey: string, businessEventId: string): void {
        if (idempotencyKey !== businessEventId) {
            throw new ImGatewayError('duplicate_event', 'Idempotency-Key must equal the contract business event ID');
        }
    }
}

/** The real Koishi Server route serializes this AsyncIterable as SSE frames. */
export interface ReminderActionSseEvent {
    readonly id: ActionId;
    readonly event: 'reminder.action';
    readonly data: ReminderActionCommand;
}

export class ReminderActionStreamController {
    public constructor(
        private readonly stream: ActionCommandStreamPort,
        private readonly authentication: DeviceAuthenticationPort,
        private readonly actions: ActionApplication,
    ) {}

    public async connect(input: {
        readonly authorization: string;
        readonly deviceId: DeviceId;
        readonly reminderType: 'strong';
        readonly reminderTriggerId: ReminderTriggerId;
        readonly lastEventId?: ActionId;
        readonly signal?: AbortSignal;
    }): Promise<AsyncIterable<ReminderActionSseEvent>> {
        const principal = await this.authentication.authenticate(input.authorization);
        if (principal.deviceId !== input.deviceId) {
            throw new ImGatewayError('invalid_transition', 'Device token is not bound to the requested deviceId');
        }
        await this.actions.expireDue();
        const expiresAt = await this.actions.resolveActionWindow(input.deviceId, input.reminderTriggerId);
        const replay = await this.actions.replayPending(input.deviceId, input.reminderTriggerId, input.lastEventId);
        const replayCursor = replay.at(-1)?.commandId ?? input.lastEventId;
        const live = this.stream.subscribe({
            deviceId: input.deviceId,
            reminderTriggerId: input.reminderTriggerId,
            expiresAt,
            ...(replayCursor === undefined ? {} : { lastEventId: replayCursor }),
            ...(input.signal === undefined ? {} : { signal: input.signal }),
        });
        return markCommandsProcessing(concatenateCommands(replay, live), this.actions);
    }
}

async function* concatenateCommands(
    replay: readonly ReminderActionCommand[],
    live: AsyncIterable<ReminderActionCommand>,
): AsyncIterable<ReminderActionCommand> {
    for (const command of replay) yield command;
    for await (const command of live) yield command;
}

async function* markCommandsProcessing(
    commands: AsyncIterable<ReminderActionCommand>,
    actions: ActionApplication,
): AsyncIterable<ReminderActionSseEvent> {
    for await (const command of commands) {
        await actions.markProcessing(command.commandId, command.deviceId, command.reminderTriggerId);
        yield {
            id: command.commandId,
            event: 'reminder.action',
            data: command,
        };
    }
}

export const SSE_RESPONSE_HEADERS = {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache',
    'X-Accel-Buffering': 'no',
} as const;

export const SSE_HEARTBEAT_INTERVAL_SECONDS = 20;
