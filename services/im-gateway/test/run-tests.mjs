import { readFile } from 'node:fs/promises';

import {
    ImGatewayError,
    createMockImGateway,
    parseCreatePairingSessionRequest,
    parseNotificationIntent,
    parseReminderActionIntent,
    parseReminderActionResult,
    parseScheduleReceiptIntent,
    parseScheduleQueryResultIntent,
    parseVoiceReminderActionStatus,
    runMockNotificationScenario,
} from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';

const fixtureRoot = new URL('../../../contracts/im-gateway/v1/fixtures/', import.meta.url);

function assert(condition, message) {
    if (!condition) throw new Error(message);
}

async function readFixture(name) {
    return JSON.parse(await readFile(new URL(name, fixtureRoot), 'utf8'));
}

async function expectGatewayError(work, code, message) {
    try {
        await work();
    } catch (error) {
        if (error instanceof ImGatewayError && error.code === code) return;
        throw error;
    }
    throw new Error(message);
}

async function expectRejected(work, message) {
    try {
        await work();
    } catch (error) {
        return error;
    }
    throw new Error(message);
}

async function createBoundGateway(overrides = {}) {
    const clock = new FixedClock();
    const gateway = createMockImGateway('device-fixture', clock, overrides);
    const channel = await gateway.application.channels.register({
        platform: 'wechat_official',
        tenantExternalId: 'fixture-account',
        koishiBotId: 'fixture-bot',
        credentialRef: 'secret://fixture-account',
        connectionMode: 'webhook',
    });
    const pairing = await gateway.deviceApi.postPairingSession({
        authorization: 'Bearer fixture-device-token',
        body: { userId: 'user-fixture', deviceId: 'device-fixture' },
    });
    await gateway.application.pairing.confirm({
        displayCode: pairing.displayCode,
        channelAccountId: channel.id,
        externalUserId: 'fixture-open-id',
    });
    return { gateway, clock };
}

async function submitFixture(gateway, body) {
    return gateway.deviceApi.postNotification({
        authorization: 'Bearer fixture-device-token',
        idempotencyKey: body.businessEventId,
        body,
    });
}

async function runContractFixtureTests() {
    const pairingRequest = await readFixture('pairing-create-request.json');
    const minimalPairingRequest = await readFixture('pairing-create-request-minimal.json');
    const expiredPairing = await readFixture('pairing-status-expired.json');
    const cancelledPairing = await readFixture('pairing-status-cancelled.json');
    const scheduleReceipt = await readFixture('schedule-receipt.json');
    const scheduleQueryResult = await readFixture('schedule-query-result.json');
    const strong = await readFixture('notification-strong.json');
    const replay = await readFixture('notification-strong-replay.json');
    const weak = await readFixture('notification-weak.json');
    const conflict = await readFixture('notification-conflict.json');
    const actionResult = await readFixture('reminder-action-result.json');
    const voiceStatus = await readFixture('voice-reminder-action-status.json');

    assert(
        parseCreatePairingSessionRequest(pairingRequest).expiresInMinutes === 5,
        'Pairing create request fixture did not preserve expiresInMinutes',
    );
    assert(
        parseCreatePairingSessionRequest(minimalPairingRequest).deviceId === 'device-fixture',
        'Minimal pairing create request fixture did not parse',
    );
    assert(
        expiredPairing.status === 'expired' && cancelledPairing.status === 'cancelled',
        'Pairing terminal fixtures must preserve the public status enum',
    );
    for (const name of ['pairing-create-request-invalid-expiry.json', 'pairing-create-request-invalid-platform.json']) {
        const invalidPairingRequest = await readFixture(name);
        await expectGatewayError(
            () => Promise.resolve(parseCreatePairingSessionRequest(invalidPairingRequest)),
            'invalid_contract',
            `${name} was accepted by the runtime parser`,
        );
    }

    assert(
        parseScheduleReceiptIntent(scheduleReceipt).scheduleId === 'schedule-fixture',
        'ScheduleReceiptIntent fixture did not preserve its opaque string ID',
    );
    assert(
        parseScheduleQueryResultIntent(scheduleQueryResult).resultCount === 2,
        'ScheduleQueryResultIntent fixture did not preserve the complete result count',
    );
    const minimalScheduleQueryResult = JSON.parse(JSON.stringify(scheduleQueryResult));
    delete minimalScheduleQueryResult.userId;
    minimalScheduleQueryResult.query = { status: 'all' };
    minimalScheduleQueryResult.resultCount = 0;
    minimalScheduleQueryResult.schedules = [];
    minimalScheduleQueryResult.futureOccurrences = [];
    minimalScheduleQueryResult.exceptions = [];
    assert(
        parseScheduleQueryResultIntent(minimalScheduleQueryResult).query.status === 'all',
        'ScheduleQueryResultIntent must accept an empty result without optional query filters',
    );
    for (const invalidScheduleQueryResult of [
        { ...scheduleQueryResult, query: { ...scheduleQueryResult.query, status: 'unknown' } },
        { ...scheduleQueryResult, query: { ...scheduleQueryResult.query, startDate: '2026/08/03' } },
        { ...scheduleQueryResult, query: { ...scheduleQueryResult.query, endDate: '2026-08' } },
        { ...scheduleQueryResult, resultCount: 1.5 },
        { ...scheduleQueryResult, resultCount: 1001 },
        { ...scheduleQueryResult, schedules: {} },
        { ...scheduleQueryResult, futureOccurrences: {} },
        { ...scheduleQueryResult, exceptions: {} },
        { ...scheduleQueryResult, schedules: Array.from({ length: 101 }, () => ({ id: 1 })) },
        { ...scheduleQueryResult, schedules: [undefined] },
        { ...scheduleQueryResult, resultCount: 0 },
    ]) {
        await expectGatewayError(
            () => Promise.resolve(parseScheduleQueryResultIntent(invalidScheduleQueryResult)),
            'invalid_contract',
            'Invalid ScheduleQueryResultIntent was accepted by the runtime parser',
        );
    }
    assert(parseNotificationIntent(strong).reminderType === 'strong', 'Strong notification fixture did not parse');
    assert(
        parseNotificationIntent(weak).actions.length === 0,
        'Weak notification fixture did not parse without actions',
    );

    for (const name of [
        'notification-invalid-version.json',
        'notification-invalid-enum.json',
        'notification-invalid-time.json',
        'notification-missing-field.json',
    ]) {
        const invalidFixture = await readFixture(name);
        await expectGatewayError(
            () => Promise.resolve(parseNotificationIntent(invalidFixture)),
            'invalid_contract',
            `${name} was accepted by the runtime parser`,
        );
    }

    for (const name of [
        'schedule-receipt-invalid-version.json',
        'schedule-receipt-invalid-enum.json',
        'schedule-receipt-invalid-time.json',
    ]) {
        const invalidScheduleReceipt = await readFixture(name);
        await expectGatewayError(
            () => Promise.resolve(parseScheduleReceiptIntent(invalidScheduleReceipt)),
            'invalid_contract',
            `${name} was accepted by the runtime parser`,
        );
    }

    const parsedActionResult = parseReminderActionResult(actionResult);
    assert(
        parsedActionResult.status === 'succeeded' &&
            parsedActionResult.operationId === 'operation-fixture' &&
            parsedActionResult.nextTriggerAt === '2026-08-03T00:10:00.000Z',
        'ReminderActionResult fixture did not preserve the execution result',
    );
    for (const name of ['reminder-action-result-invalid-status.json', 'reminder-action-result-invalid-time.json']) {
        const invalidActionResult = await readFixture(name);
        await expectGatewayError(
            () => Promise.resolve(parseReminderActionResult(invalidActionResult)),
            'invalid_contract',
            `${name} was accepted by the runtime parser`,
        );
    }

    const parsedVoiceStatus = parseVoiceReminderActionStatus(voiceStatus);
    assert(
        parsedVoiceStatus.action === 'snooze' &&
            parsedVoiceStatus.status === 'succeeded' &&
            parsedVoiceStatus.operationId === 'voice-operation-fixture' &&
            parsedVoiceStatus.nextTriggerAt === '2026-08-03T00:11:00.000Z',
        'VoiceReminderActionStatus fixture did not preserve the voice action fact',
    );
    await expectGatewayError(
        async () =>
            parseVoiceReminderActionStatus(await readFixture('voice-reminder-action-status-invalid-source.json')),
        'invalid_contract',
        'VoiceReminderActionStatus accepted a non-voice source',
    );

    await expectGatewayError(
        () =>
            Promise.resolve(
                parseReminderActionResult({
                    schemaVersion: '1',
                    operationId: 'operation-fixture',
                    reminderTriggerId: 'trigger-fixture',
                    status: 'succeeded',
                    occurredAt: 'not-a-time',
                }),
            ),
        'invalid_contract',
        'ReminderActionResult accepted an invalid ISO 8601 value',
    );
    await expectGatewayError(
        () =>
            Promise.resolve(
                parseReminderActionResult({
                    schemaVersion: '1',
                    operationId: 'operation-fixture',
                    reminderTriggerId: 'trigger-fixture',
                    status: 'succeeded',
                    occurredAt: '2026-02-31T00:00:00.000Z',
                }),
            ),
        'invalid_contract',
        'ReminderActionResult accepted an impossible calendar date',
    );
    await expectGatewayError(
        () =>
            Promise.resolve(
                parseReminderActionIntent({
                    token: 'fixture-token',
                    action: 'snooze',
                    params: { minutes: 0 },
                }),
            ),
        'invalid_contract',
        'Action UI accepted a non-positive snooze duration',
    );
    await expectGatewayError(
        () =>
            Promise.resolve(
                parseReminderActionIntent({
                    token: 'fixture-token',
                    action: 'snooze',
                    params: { minutes: 1441 },
                }),
            ),
        'invalid_contract',
        'Action UI accepted a snooze duration above the device limit',
    );
    await expectGatewayError(
        () =>
            Promise.resolve(
                parseNotificationIntent({
                    ...strong,
                    actions: Array.from({ length: 17 }, () => ({
                        kind: 'command',
                        type: 'acknowledge',
                        label: 'Acknowledge',
                    })),
                }),
            ),
        'invalid_contract',
        'NotificationIntent accepted more actions than the device limit',
    );
    for (const label of ['', '   ', 'bad\nlabel', 'bad\u202Elabel', 'x'.repeat(129)]) {
        await expectGatewayError(
            () =>
                Promise.resolve(
                    parseNotificationIntent({
                        ...strong,
                        actions: [{ kind: 'command', type: 'acknowledge', label }],
                    }),
                ),
            'invalid_contract',
            `NotificationIntent accepted unsafe action label ${JSON.stringify(label)}`,
        );
    }

    const { gateway } = await createBoundGateway();
    const first = await submitFixture(gateway, strong);
    const duplicate = await submitFixture(gateway, replay);
    assert(
        first.deliveries[0]?.deliveryId === duplicate.deliveries[0]?.deliveryId,
        'Identical fixture replay did not return the original submission',
    );
    await expectGatewayError(
        () => submitFixture(gateway, conflict),
        'idempotency_conflict',
        'Conflicting fixture replay was accepted',
    );
}

async function runFailureStateTests() {
    const strong = await readFixture('notification-strong.json');
    const { gateway: deliveryGateway } = await createBoundGateway({
        imChannel: {
            send: async () => {
                throw new Error('fixture channel outage');
            },
        },
    });
    const submitted = await submitFixture(deliveryGateway, strong);
    const deliveryId = submitted.deliveries[0]?.deliveryId;
    assert(deliveryId !== undefined, 'Failure fixture did not create a Delivery');
    const failed = await deliveryGateway.application.deliveryDispatch.dispatch(deliveryId);
    const details = await deliveryGateway.application.deliveries.find(deliveryId);
    assert(
        failed.status === 'retryable_failed' && details?.attempts[0]?.status === 'retryable_failed',
        'Thrown channel.send() did not move Delivery and Attempt to retryable_failed',
    );

    const failingStream = {
        publish: async () => {
            throw new Error('fixture stream outage');
        },
        subscribe: () => (async function* emptyStream() {})(),
        close: async () => {},
    };
    const { gateway: actionGateway } = await createBoundGateway({
        actionStream: failingStream,
    });
    const actionSubmission = await submitFixture(actionGateway, strong);
    const actionDeliveryId = actionSubmission.deliveries[0]?.deliveryId;
    assert(actionDeliveryId !== undefined, 'Action fixture did not create a Delivery');
    await actionGateway.application.deliveryDispatch.dispatch(actionDeliveryId);
    const token = await actionGateway.application.actionUi.issue(actionDeliveryId);
    const streamError = await expectRejected(
        () => actionGateway.actionUiApi.post({ token, action: 'acknowledge' }),
        'Action stream fixture did not throw',
    );
    assert(
        streamError instanceof Error && streamError.message === 'fixture stream outage',
        'Action stream failure was not propagated',
    );
    const replayable = await actionGateway.application.actions.replayPending('device-fixture', 'trigger-fixture');
    assert(replayable.length === 1, 'A dispatched Action was not recoverable after stream.publish() failed');

    const publishedCommands = [];
    const recordingStream = {
        publish: async (command) => {
            publishedCommands.push(command);
        },
        subscribe: () => (async function* emptyStream() {})(),
        close: async () => {},
    };
    const { gateway: retryGateway } = await createBoundGateway({
        actionStream: recordingStream,
    });
    const retrySubmission = await submitFixture(retryGateway, strong);
    const retryDeliveryId = retrySubmission.deliveries[0]?.deliveryId;
    assert(retryDeliveryId !== undefined, 'Retry fixture did not create a Delivery');
    await retryGateway.application.deliveryDispatch.dispatch(retryDeliveryId);
    const retryToken = await retryGateway.application.actionUi.issue(retryDeliveryId);
    const retryCommand = await retryGateway.actionUiApi.post({
        token: retryToken,
        action: 'acknowledge',
    });
    await retryGateway.deviceApi.postReminderActionResult({
        authorization: 'Bearer fixture-device-token',
        deviceId: 'device-fixture',
        commandId: retryCommand.commandId,
        body: {
            schemaVersion: '1',
            operationId: retryCommand.operationId,
            reminderTriggerId: retryCommand.reminderTriggerId,
            status: 'retryable_failed',
            occurredAt: '2026-08-03T00:01:00.000Z',
        },
    });
    assert(
        publishedCommands.length === 2 && publishedCommands[0]?.operationId === publishedCommands[1]?.operationId,
        'retryable_failed did not republish the same operation to the live stream',
    );
}

async function runIssue65TransportBoundaryTests() {
    const strong = await readFixture('notification-strong.json');
    const { gateway } = await createBoundGateway();
    await submitFixture(gateway, strong);

    await expectGatewayError(
        () =>
            gateway.deviceApi.postNotification({
                authorization: 'Bearer fixture-device-token',
                idempotencyKey: 'another-event-id',
                body: strong,
            }),
        'duplicate_event',
        'A notification with an Idempotency-Key different from businessEventId was accepted',
    );

    await expectGatewayError(
        () =>
            gateway.actionStreamApi.connect({
                authorization: 'Bearer fixture-device-token',
                deviceId: 'another-device',
                reminderType: 'strong',
                reminderTriggerId: 'trigger-fixture',
            }),
        'invalid_transition',
        "A device token was allowed to open another device's action stream",
    );

    await expectGatewayError(
        () =>
            gateway.actionStreamApi.connect({
                authorization: 'Bearer fixture-device-token',
                deviceId: 'device-fixture',
                reminderType: 'weak',
                reminderTriggerId: 'trigger-fixture',
            }),
        'invalid_contract',
        'A weak reminder was allowed to establish the strong-reminder SSE action stream',
    );
}

async function runOutboundGenerationTests() {
    // 生成测试：网关产出（NotificationSubmission / ReminderActionCommand）必须符合
    // 共享 outbound fixture 的结构；generated 的 deliveryId/bindingId 仅断言类型，
    // 其余字段与 fixture 逐项对齐（见 manifest 的 outbound 契约）。
    const submissionSpec = await readFixture('notification-submission.json');
    const weakSubmissionSpec = await readFixture('notification-submission-weak.json');
    const commandSpec = await readFixture('reminder-action-command.json');
    const pairingRequest = await readFixture('pairing-create-request.json');
    const pairingCreatedSpec = await readFixture('pairing-created.json');
    const pendingPairingSpec = await readFixture('pairing-status.json');
    const confirmedPairingSpec = await readFixture('pairing-status-confirmed.json');

    const strong = await readFixture('notification-strong.json');
    const weak = await readFixture('notification-weak.json');
    const { gateway } = await createBoundGateway();

    const pairingGateway = createMockImGateway('device-fixture', new FixedClock());
    const createdPairing = await pairingGateway.deviceApi.postPairingSession({
        authorization: 'Bearer fixture-device-token',
        body: pairingRequest,
    });
    assert(
        JSON.stringify(createdPairing) === JSON.stringify(pairingCreatedSpec),
        'CreatedPairingSession 生成与 pairing-created.json 不一致',
    );
    const pendingPairing = await pairingGateway.deviceApi.getPairingSession({
        authorization: 'Bearer fixture-device-token',
        pairingSessionId: createdPairing.session.id,
    });
    assert(
        JSON.stringify(pendingPairing) === JSON.stringify(pendingPairingSpec),
        'PairingSessionStatus 生成与 pairing-status.json 不一致',
    );
    const pairingChannel = await pairingGateway.application.channels.register({
        platform: 'wechat_official',
        tenantExternalId: 'fixture-account',
        koishiBotId: 'fixture-bot',
        credentialRef: 'secret://fixture-account',
        connectionMode: 'webhook',
    });
    await pairingGateway.application.pairing.confirm({
        displayCode: createdPairing.displayCode,
        channelAccountId: pairingChannel.id,
        externalUserId: 'fixture-open-id',
    });
    const confirmedPairing = await pairingGateway.deviceApi.getPairingSession({
        authorization: 'Bearer fixture-device-token',
        pairingSessionId: createdPairing.session.id,
    });
    assert(
        JSON.stringify(confirmedPairing) === JSON.stringify(confirmedPairingSpec),
        'Confirmed PairingSessionStatus 生成与 pairing-status-confirmed.json 不一致',
    );

    const submission = await submitFixture(gateway, strong);
    assert(
        submission.businessEventId === submissionSpec.businessEventId &&
            submission.status === submissionSpec.status &&
            submission.deliveries.length === submissionSpec.deliveries.length &&
            submission.deliveries.every((delivery) => delivery.status === 'pending') &&
            typeof submission.deliveries[0].bindingId === 'string' &&
            typeof submission.deliveries[0].deliveryId === 'string' &&
            JSON.stringify(submission.actionStream) === JSON.stringify(submissionSpec.actionStream),
        'NotificationSubmission 生成与 notification-submission.json 结构不一致',
    );

    // 应用层无绑定 userId → 空 deliveries；设备 API 会额外拒绝冒充其他 userId。
    const emptySubmission = await gateway.application.notifications.submitNotification({
        ...weak,
        recipient: { userId: 'unbound-user', deviceId: 'device-fixture' },
    });
    assert(
        JSON.stringify(emptySubmission) === JSON.stringify(weakSubmissionSpec),
        'NotificationSubmission 生成与 notification-submission-weak.json 不一致',
    );

    const publishedCommands = [];
    const recordingStream = {
        publish: async (command) => {
            publishedCommands.push(command);
        },
        subscribe: () => (async function* emptyStream() {})(),
        close: async () => {},
    };
    const { gateway: actionGateway } = await createBoundGateway({ actionStream: recordingStream });
    const actionSubmission = await submitFixture(actionGateway, strong);
    const actionDeliveryId = actionSubmission.deliveries[0]?.deliveryId;
    assert(actionDeliveryId !== undefined, 'Outbound action fixture did not create a Delivery');
    await actionGateway.application.deliveryDispatch.dispatch(actionDeliveryId);
    const token = await actionGateway.application.actionUi.issue(actionDeliveryId);
    // 生成与 fixture 一致的 snooze + minutes=10 场景；仅归一化真正动态的 ID 后整体深比较。
    await actionGateway.actionUiApi.post({ token, action: 'snooze', params: { minutes: 10 } });
    const command = publishedCommands[0];
    const normalizedCommand = {
        ...commandSpec,
        commandId: command.commandId,
        operationId: command.operationId,
        actorBindingId: command.actorBindingId,
    };
    assert(
        JSON.stringify(command) === JSON.stringify(normalizedCommand),
        'ReminderActionCommand 生成与 reminder-action-command.json 不一致',
    );
}

await runMockNotificationScenario();
await runContractFixtureTests();
await runFailureStateTests();
await runIssue65TransportBoundaryTests();
await runOutboundGenerationTests();
console.log('IM Gateway Issue #65/#95 contract and regression tests passed');
