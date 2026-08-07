import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    DEVICE_API_ENDPOINTS,
    DEVICE_API_ROUTES,
    ReminderActionStreamController,
    SSE_HEARTBEAT_INTERVAL_SECONDS,
    SSE_RESPONSE_HEADERS,
    SseActionCommandHub,
} from '../dist/index.js';
import { buildGateway, expectGatewayError, strongIntent } from './helpers.mjs';

test('device route metadata matches the Issue #65 transport contract', () => {
    assert.equal(DEVICE_API_ROUTES.pairingSessions, '/v1/im/pairing-sessions');
    assert.equal(DEVICE_API_ROUTES.scheduleReceipts, '/v1/im/schedule-receipts');
    assert.equal(DEVICE_API_ROUTES.notifications, '/v1/im/notifications');
    assert.equal(DEVICE_API_ROUTES.reminderActionStream, '/v1/devices/:deviceId/reminder-actions/stream');
    assert.equal(DEVICE_API_ROUTES.reminderActionResults, '/v1/devices/:deviceId/reminder-actions/:commandId/result');
    assert.deepEqual(DEVICE_API_ENDPOINTS.notification, {
        method: 'POST',
        path: '/v1/im/notifications',
        transport: 'https',
    });
    assert.deepEqual(SSE_RESPONSE_HEADERS, {
        'Content-Type': 'text/event-stream',
        'Cache-Control': 'no-cache',
        'X-Accel-Buffering': 'no',
    });
    assert.equal(SSE_HEARTBEAT_INTERVAL_SECONDS >= 15 && SSE_HEARTBEAT_INTERVAL_SECONDS <= 30, true);
});

test('notification rejects an Idempotency-Key different from businessEventId', async () => {
    const { gateway } = buildGateway();

    await expectGatewayError(
        () =>
            gateway.deviceApi.postNotification({
                authorization: 'Bearer fixture-device-token',
                idempotencyKey: 'event-other',
                body: strongIntent(),
            }),
        'duplicate_event',
        'A mismatched notification Idempotency-Key was accepted',
    );
});

test('pairing creation parses untrusted device input before authentication', async () => {
    const { gateway } = buildGateway();
    const invalidBodies = [
        null,
        {},
        { deviceId: '' },
        { deviceId: 'device-fixture', allowedPlatforms: ['unknown'] },
        { deviceId: 'device-fixture', expiresInMinutes: 0 },
        { deviceId: 'device-fixture', expiresInMinutes: 11 },
    ];

    for (const body of invalidBodies) {
        await expectGatewayError(
            () => gateway.deviceApi.postPairingSession({ authorization: 'Bearer fixture-device-token', body }),
            'invalid_contract',
            `Pairing accepted invalid body: ${JSON.stringify(body)}`,
        );
    }
});

test('notification rejects a body for another device principal', async () => {
    const { gateway } = buildGateway();

    await expectGatewayError(
        () =>
            gateway.deviceApi.postNotification({
                authorization: 'Bearer fixture-device-token',
                idempotencyKey: 'event-other-device',
                body: strongIntent({
                    businessEventId: 'event-other-device',
                    recipient: { userId: 'user-fixture', deviceId: 'device-other' },
                }),
            }),
        'invalid_transition',
        'A device principal submitted a notification for another device',
    );
});

test('pairing status is hidden from a different device principal', async () => {
    const { gateway } = buildGateway();
    const created = await gateway.application.pairing.create({
        userId: 'user-fixture',
        deviceId: 'device-other',
    });

    const result = await gateway.deviceApi.getPairingSession({
        authorization: 'Bearer fixture-device-token',
        pairingSessionId: created.session.id,
    });

    assert.equal(result, undefined);
});

test('action result rejects a path for another device principal', async () => {
    const { gateway } = buildGateway();

    await expectGatewayError(
        () =>
            gateway.deviceApi.postReminderActionResult({
                authorization: 'Bearer fixture-device-token',
                deviceId: 'device-other',
                commandId: 'action-missing',
                body: {
                    schemaVersion: '1',
                    operationId: 'operation-fixture',
                    reminderTriggerId: 'trigger-fixture',
                    status: 'failed',
                    occurredAt: '2026-08-03T00:00:00.000Z',
                },
            }),
        'invalid_transition',
        'A device principal posted an action result for another device path',
    );
});

test('action stream rejects weak reminders at runtime', async () => {
    const { gateway } = buildGateway();

    await expectGatewayError(
        () =>
            gateway.actionStreamApi.connect({
                authorization: 'Bearer fixture-device-token',
                deviceId: 'device-fixture',
                reminderType: 'weak',
                reminderTriggerId: 'trigger-fixture',
            }),
        'invalid_contract',
        'A weak reminder established the strong-reminder action stream',
    );
});

test('action stream rejects a path for another device principal', async () => {
    const { gateway } = buildGateway();

    await expectGatewayError(
        () =>
            gateway.actionStreamApi.connect({
                authorization: 'Bearer fixture-device-token',
                deviceId: 'device-other',
                reminderType: 'strong',
                reminderTriggerId: 'trigger-fixture',
            }),
        'invalid_transition',
        'A device principal established another device action stream',
    );
});

test('action stream does not lose a command published between persistent replay and live delivery', async () => {
    const hub = new SseActionCommandHub();
    let markProcessingCalls = 0;
    let releaseReplay;
    let reportReplayStarted;
    const replayGate = new Promise((resolve) => {
        releaseReplay = resolve;
    });
    const replayStarted = new Promise((resolve) => {
        reportReplayStarted = resolve;
    });
    const command = {
        schemaVersion: '1',
        commandId: 'action-race',
        operationId: 'operation-race',
        correlationId: 'correlation-race',
        deviceId: 'device-fixture',
        reminderTriggerId: 'trigger-fixture',
        action: 'acknowledge',
        issuedAt: '2026-08-07T00:00:00.000Z',
        expiresAt: '2099-08-07T00:10:00.000Z',
    };
    const controller = new ReminderActionStreamController(
        hub,
        { authenticate: async () => ({ deviceId: 'device-fixture' }) },
        {
            expireDue: async () => 0,
            resolveActionWindow: async () => command.expiresAt,
            replayPending: async () => {
                reportReplayStarted();
                await replayGate;
                return [];
            },
            markProcessing: async () => {
                markProcessingCalls += 1;
            },
        },
    );
    const connecting = controller.connect({
        authorization: 'Bearer fixture-device-token',
        deviceId: 'device-fixture',
        reminderType: 'strong',
        reminderTriggerId: 'trigger-fixture',
        signal: globalThis.AbortSignal.timeout(50),
    });

    await replayStarted;
    await hub.publish(command);
    releaseReplay();
    const stream = await connecting;

    assert.deepEqual(await stream[Symbol.asyncIterator]().next(), {
        done: false,
        value: { id: command.commandId, event: 'reminder.action', data: command },
    });
    assert.equal(markProcessingCalls, 1);
});
