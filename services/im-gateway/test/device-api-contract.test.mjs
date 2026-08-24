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
import {
    buildGateway,
    expectGatewayError,
    pendingStrongDelivery,
    scheduleQueryResultIntent,
    strongIntent,
} from './helpers.mjs';

test('device route metadata matches the Issue #65 transport contract', () => {
    assert.equal(DEVICE_API_ROUTES.pairingSessions, '/v1/im/pairing-sessions');
    assert.equal(DEVICE_API_ROUTES.scheduleReceipts, '/v1/im/schedule-receipts');
    assert.equal(DEVICE_API_ROUTES.scheduleQueryResults, '/v1/im/schedule-query-results');
    assert.equal(DEVICE_API_ROUTES.notifications, '/v1/im/notifications');
    assert.equal(DEVICE_API_ROUTES.voiceReminderActionStatuses, '/v1/im/reminder-action-statuses');
    assert.equal(DEVICE_API_ROUTES.reminderActionStream, '/v1/devices/:deviceId/reminder-actions/stream');
    assert.equal(DEVICE_API_ROUTES.reminderActionResults, '/v1/devices/:deviceId/reminder-actions/:commandId/result');
    assert.deepEqual(DEVICE_API_ENDPOINTS.notification, {
        method: 'POST',
        path: '/v1/im/notifications',
        transport: 'https',
    });
    assert.deepEqual(DEVICE_API_ENDPOINTS.voiceReminderActionStatus, {
        method: 'POST',
        path: '/v1/im/reminder-action-statuses',
        transport: 'https',
    });
    assert.deepEqual(SSE_RESPONSE_HEADERS, {
        'Content-Type': 'text/event-stream',
        'Cache-Control': 'no-cache',
        'X-Accel-Buffering': 'no',
    });
    assert.equal(SSE_HEARTBEAT_INTERVAL_SECONDS >= 15 && SSE_HEARTBEAT_INTERVAL_SECONDS <= 30, true);
});

test('voice reminder status is idempotent and closes the pending H5 action', async () => {
    const { gateway } = buildGateway();
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    const token = await gateway.application.actionUi.issue(deliveryId);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const body = {
        schemaVersion: '1',
        eventId: 'voice-event-fixture',
        correlationId: 'voice-correlation-fixture',
        deviceId: 'device-fixture',
        reminderTriggerId: command.reminderTriggerId,
        operationId: 'voice-operation-fixture',
        action: 'acknowledge',
        status: 'succeeded',
        occurredAt: '2026-08-03T00:01:00.000Z',
        source: 'voice',
    };
    const first = await gateway.deviceApi.postVoiceReminderActionStatus({
        authorization: 'Bearer fixture-device-token',
        idempotencyKey: body.eventId,
        body,
    });
    assert.equal(first[0].status, 'succeeded');
    const replay = await gateway.deviceApi.postVoiceReminderActionStatus({
        authorization: 'Bearer fixture-device-token',
        idempotencyKey: body.eventId,
        body,
    });
    assert.deepEqual(replay, first);
    assert.equal((await gateway.application.actionUi.show(token)).state, 'succeeded');
    await expectGatewayError(
        () =>
            gateway.deviceApi.postVoiceReminderActionStatus({
                authorization: 'Bearer fixture-device-token',
                idempotencyKey: 'other-event',
                body,
            }),
        'duplicate_event',
        'A mismatched voice status Idempotency-Key was accepted',
    );
});

test('voice reminder status materializes an action when H5 was never opened', async () => {
    const { gateway, unitOfWork } = buildGateway();
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    const body = {
        schemaVersion: '1',
        eventId: 'voice-event-without-h5',
        correlationId: 'voice-correlation-without-h5',
        deviceId: 'device-fixture',
        reminderTriggerId: 'trigger-fixture',
        operationId: 'voice-operation-without-h5',
        action: 'snooze',
        status: 'succeeded',
        nextTriggerAt: '2026-08-03T00:11:00.000Z',
        occurredAt: '2026-08-03T00:01:00.000Z',
        source: 'voice',
    };
    const result = await gateway.deviceApi.postVoiceReminderActionStatus({
        authorization: 'Bearer fixture-device-token',
        idempotencyKey: body.eventId,
        body,
    });
    assert.equal(result[0].status, 'succeeded');
    const token = await gateway.application.actionUi.issue(deliveryId);
    assert.equal((await gateway.application.actionUi.show(token)).state, 'succeeded');
    const stored = await unitOfWork.transaction((tx) => tx.actions.findByOperationId(body.operationId));
    assert.deepEqual(stored?.result?.details, {
        source: 'voice',
        correlationId: body.correlationId,
    });
    assert.equal(stored?.deliveryId, deliveryId);
});

test('schedule query result enforces device identity and idempotency', async () => {
    const { gateway } = buildGateway();
    const body = scheduleQueryResultIntent();
    const first = await gateway.deviceApi.postScheduleQueryResult({
        authorization: 'Bearer fixture-device-token',
        idempotencyKey: body.businessEventId,
        body,
    });
    const replay = await gateway.deviceApi.postScheduleQueryResult({
        authorization: 'Bearer fixture-device-token',
        idempotencyKey: body.businessEventId,
        body,
    });
    assert.deepEqual(replay, first);
    await expectGatewayError(
        () =>
            gateway.deviceApi.postScheduleQueryResult({
                authorization: 'Bearer fixture-device-token',
                idempotencyKey: 'other-event',
                body,
            }),
        'duplicate_event',
        'A mismatched query result Idempotency-Key was accepted',
    );
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
        { deviceId: 'device-fixture', allowedPlatforms: [] },
        { deviceId: 'device-fixture', allowedPlatforms: ['wechat_official', 'wechat_official'] },
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

test('pairing device responses never expose the internal display code hash', async () => {
    const { gateway } = buildGateway();
    const created = await gateway.deviceApi.postPairingSession({
        authorization: 'Bearer fixture-device-token',
        body: {
            userId: 'user-fixture',
            deviceId: 'device-fixture',
            allowedPlatforms: ['wechat_official'],
            expiresInMinutes: 5,
        },
    });
    assert.equal('displayCodeHash' in created.session, false);
    assert.deepEqual(Object.keys(created.session).sort(), [
        'allowedPlatforms',
        'createdAt',
        'deviceId',
        'expiresAt',
        'id',
        'status',
        'userId',
    ]);

    const status = await gateway.deviceApi.getPairingSession({
        authorization: 'Bearer fixture-device-token',
        pairingSessionId: created.session.id,
    });
    assert.equal(status === undefined ? undefined : 'displayCodeHash' in status, false);
});

test('pairing creation derives userId from the authenticated device principal', async () => {
    const { gateway } = buildGateway();
    const created = await gateway.deviceApi.postPairingSession({
        authorization: 'Bearer fixture-device-token',
        body: { deviceId: 'device-fixture', allowedPlatforms: ['wechat_official'] },
    });
    assert.equal(created.session.userId, 'user-fixture');

    await expectGatewayError(
        () =>
            gateway.deviceApi.postPairingSession({
                authorization: 'Bearer fixture-device-token',
                body: { deviceId: 'device-fixture', userId: 'user-other' },
            }),
        'invalid_transition',
        'Pairing accepted a userId that was not bound to the device credential',
    );
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

test('an unknown device cannot create a pairing session', async () => {
    const { gateway } = buildGateway();
    await expectGatewayError(
        () => gateway.application.pairing.create({ userId: 'user-fixture', deviceId: 'device-other' }),
        'unauthorized',
        'An unregistered device created a pairing session',
    );
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
