import { test } from 'node:test';
import assert from 'node:assert/strict';

import { createImGateway, createMockImGateway, mockImGatewayPorts } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import {
    bindFixtureUser,
    buildGateway,
    expectRejected,
    pendingStrongDelivery,
    seedDevice,
    strongIntent,
    weakIntent,
} from './helpers.mjs';

/** 记录发布命令与关闭动作的动作流 Port。 */
function recordingActionStream() {
    const commands = [];
    const closed = [];
    return {
        commands,
        closed,
        port: {
            publish: async (command) => {
                commands.push(command);
            },
            subscribe: async function* () {},
            close: async (actionId) => {
                closed.push(actionId);
            },
        },
    };
}

/** 构建 Gateway 并注入记录型动作流,返回可读的时钟与流。 */
function actionGateway(overrides = {}) {
    const clock = new FixedClock();
    const stream = recordingActionStream();
    const gateway = createMockImGateway('device-fixture', clock, { actionStream: stream.port, ...overrides });
    return { gateway, clock, stream };
}

/** 提交一条强提醒投递并签发动作令牌。 */
async function prepareAction(gateway) {
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    const token = await gateway.application.actionUi.issue(deliveryId);
    return { deliveryId, token };
}

/** 构造与动作命令匹配的设备成功回执。 */
function succeededResult(command, occurredAt, overrides = {}) {
    return {
        schemaVersion: '1',
        operationId: command.operationId,
        reminderTriggerId: command.reminderTriggerId,
        status: 'succeeded',
        occurredAt,
        ...overrides,
    };
}

function voiceReport(overrides = {}) {
    return {
        schemaVersion: '1',
        eventId: 'voice-event-fixture',
        correlationId: 'voice-correlation-fixture',
        deviceId: 'device-fixture',
        reminderTriggerId: 'trigger-fixture',
        operationId: 'voice-operation-fixture',
        action: 'acknowledge',
        status: 'succeeded',
        occurredAt: '2026-08-03T00:01:00.000Z',
        source: 'voice',
        ...overrides,
    };
}

test('a token that was never issued is rejected on show and execute', async () => {
    const { gateway } = buildGateway();
    const forged = 'mock-token:forged-action';

    const showError = await expectRejected(
        () => gateway.application.actionUi.show(forged),
        'A forged token was not rejected on show',
    );
    assert.equal(showError.code, 'action_not_found');
    const executeError = await expectRejected(
        () => gateway.application.actionUi.execute({ token: forged, action: 'acknowledge' }),
        'A forged token was not rejected on execute',
    );
    assert.equal(executeError.code, 'action_not_found');
});

test('an expired token is rejected on show', async () => {
    const { gateway, clock } = buildGateway();
    const { token } = await prepareAction(gateway);
    clock.advanceMinutes(11);

    const error = await expectRejected(
        () => gateway.application.actionUi.show(token),
        'An expired token was not rejected on show',
    );
    assert.equal(error.code, 'action_expired');
});

test('an expired token is rejected on execute', async () => {
    const { gateway, clock } = buildGateway();
    const { token } = await prepareAction(gateway);
    clock.advanceMinutes(11);

    const error = await expectRejected(
        () => gateway.application.actionUi.execute({ token, action: 'acknowledge' }),
        'An expired token was not rejected on execute',
    );
    assert.equal(error.code, 'action_expired');
});

test('a token executed by the wrong identity is rejected', async () => {
    const { gateway } = buildGateway();
    const { token } = await prepareAction(gateway);

    const error = await expectRejected(
        () =>
            gateway.application.actionUi.execute(
                { token, action: 'acknowledge' },
                { actualIdentityId: 'identity-other' },
            ),
        'A token executed by the wrong identity was not rejected',
    );
    assert.equal(error.code, 'action_expired');
});

test('action tokens cannot be shown or executed after delivery enters a terminal failure', async () => {
    const gateway = createMockImGateway('device-fixture', undefined, {
        imChannel: {
            send: async () => ({ accepted: false, retryable: false, errorCode: 'blocked' }),
        },
    });
    const deliveryId = await pendingStrongDelivery(gateway);
    const token = await gateway.application.actionUi.issue(deliveryId);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    const showError = await expectRejected(
        () => gateway.application.actionUi.show(token),
        'An action page remained available after permanent delivery failure',
    );
    assert.equal(showError.code, 'action_expired');
    const executeError = await expectRejected(
        () => gateway.application.actionUi.execute({ token, action: 'acknowledge' }),
        'An action remained executable after permanent delivery failure',
    );
    assert.equal(executeError.code, 'action_expired');
});

test('preparing a token for a delivery without an action window is rejected', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(weakIntent());
    const deliveryId = submission.deliveries[0].deliveryId;

    const error = await expectRejected(
        () => gateway.application.actions.prepareToken(deliveryId),
        'A token for a delivery without an action window was not rejected',
    );
    assert.equal(error.code, 'action_expired');
});

test('preparing a token for an active strong delivery returns the action window claims', async () => {
    const { gateway, clock } = buildGateway();
    const deliveryId = await pendingStrongDelivery(gateway);

    const claims = await gateway.application.actions.prepareToken(deliveryId);

    assert.equal(claims.actionId, `action-ui:${deliveryId}`);
    assert.equal(claims.deliveryId, deliveryId);
    assert.equal(claims.expiresAt, clock.addMinutes(clock.now(), 10));
});

test('Action UI exposes only the labels and fixed params approved by the notification', async () => {
    const { gateway } = buildGateway();
    const { token } = await prepareAction(gateway);

    const view = await gateway.application.actionUi.show(token);

    assert.deepEqual(view.actions, ['acknowledge', 'snooze']);
    assert.deepEqual(view.options, [
        { action: 'acknowledge', label: '知道了' },
        { action: 'snooze', label: '推迟 10 分钟', params: { minutes: 10 } },
    ]);
});

test('triggering a prepared acknowledge publishes a well-formed command', async () => {
    const { gateway, stream } = actionGateway();
    const { deliveryId, token } = await prepareAction(gateway);

    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });

    assert.equal(command.schemaVersion, '1');
    assert.equal(command.commandId, `action-ui:${deliveryId}`);
    assert.equal(command.action, 'acknowledge');
    assert.equal(command.deviceId, 'device-fixture');
    assert.equal(command.reminderTriggerId, 'trigger-fixture');
    assert.equal(command.params, undefined);
    assert.equal(typeof command.operationId, 'string');
    assert.equal(typeof command.expiresAt, 'string');
    const details = await gateway.application.deliveries.find(deliveryId);
    assert.equal(command.actorBindingId, details.delivery.bindingId);
    assert.equal(stream.commands.length, 1);
    assert.equal(stream.commands[0].commandId, command.commandId);
    const found = await gateway.application.actions.find(command.commandId);
    assert.equal(found.status, 'dispatched');
    assert.equal(found.actionType, 'acknowledge');
    assert.equal((await gateway.application.actions.findByOperationId(command.operationId)).id, command.commandId);
});

test('a voice-first acknowledge is persisted and consumed when IM later opens the action', async () => {
    const { gateway, stream } = actionGateway();
    await gateway.application.actions.recordDeviceActionStatus(voiceReport());

    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const action = await gateway.application.actions.find(command.commandId);

    assert.equal(action.status, 'succeeded');
    assert.equal(action.result.status, 'succeeded');
    assert.equal(stream.commands.length, 0);
});

test('a voice-first snooze preserves nextTriggerAt in the later IM action', async () => {
    const { gateway } = actionGateway();
    await gateway.application.actions.recordDeviceActionStatus(
        voiceReport({
            eventId: 'voice-snooze-event',
            operationId: 'voice-snooze-operation',
            action: 'snooze',
            nextTriggerAt: '2026-08-03T00:11:00.000Z',
        }),
    );

    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'snooze', params: { minutes: 10 } });
    const action = await gateway.application.actions.find(command.commandId);

    assert.equal(action.status, 'succeeded');
    assert.equal(action.result.nextTriggerAt, '2026-08-03T00:11:00.000Z');
});

test('a later conflicting IM action is superseded by a terminal voice fact without dispatch', async () => {
    const { gateway, stream } = actionGateway();
    await gateway.application.actions.recordDeviceActionStatus(voiceReport());

    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'snooze', params: { minutes: 10 } });
    const action = await gateway.application.actions.find(command.commandId);

    assert.equal(action.status, 'failed');
    assert.equal(action.result.status, 'failed');
    assert.equal(action.result.errorCode, 'superseded_by_device_action');
    assert.equal(stream.commands.length, 0);
});

test('a device voice report closes a pending IM action and is idempotent', async () => {
    const { gateway, stream } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const report = voiceReport({ operationId: command.operationId, correlationId: command.correlationId });

    const updated = await gateway.application.actions.recordDeviceActionStatus(report);
    const replay = await gateway.application.actions.recordDeviceActionStatus(report);

    assert.equal(updated.status, 'succeeded');
    assert.equal(replay.status, 'succeeded');
    assert.equal(stream.closed.includes(command.commandId), true);
});

test('a voice action wins over a pending conflicting IM command without a terminal rollback', async () => {
    const { gateway, clock, stream } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const voice = voiceReport({
        eventId: 'voice-snooze-wins-event',
        operationId: 'voice-snooze-wins-operation',
        correlationId: 'voice-snooze-wins-correlation',
        action: 'snooze',
        nextTriggerAt: '2026-08-03T00:11:00.000Z',
    });

    const outcome = await gateway.application.actions.recordDeviceActionStatus(voice);
    const settled = await gateway.application.actions.find(command.commandId);

    assert.equal(outcome, undefined);
    assert.equal(settled.status, 'failed');
    assert.equal(settled.result.status, 'failed');
    assert.equal(settled.result.errorCode, 'superseded_by_device_action');
    assert.equal(stream.closed.includes(command.commandId), true);
    await assert.rejects(
        () =>
            gateway.application.actions.recordResult(
                command.commandId,
                'device-fixture',
                succeededResult(command, clock.now()),
            ),
        (error) => error.code === 'invalid_transition',
    );
    const replay = await gateway.application.actions.recordDeviceActionStatus(voice);
    const unchanged = await gateway.application.actions.find(command.commandId);
    assert.equal(replay, undefined);
    assert.deepEqual(unchanged, settled);
});

test('a reused device event with different content is rejected', async () => {
    const { gateway } = actionGateway();
    await gateway.application.actions.recordDeviceActionStatus(voiceReport());

    await assert.rejects(
        () =>
            gateway.application.actions.recordDeviceActionStatus(
                voiceReport({ action: 'snooze', nextTriggerAt: '2026-08-03T00:11:00.000Z' }),
            ),
        (error) => error.code === 'idempotency_conflict',
    );
});

test('a reused device operation id with a different event is rejected', async () => {
    const { gateway } = actionGateway();
    await gateway.application.actions.recordDeviceActionStatus(voiceReport());

    await assert.rejects(
        () =>
            gateway.application.actions.recordDeviceActionStatus(
                voiceReport({
                    eventId: 'voice-event-conflict',
                    action: 'snooze',
                    operationId: 'voice-operation-fixture',
                    nextTriggerAt: '2026-08-03T00:11:00.000Z',
                }),
            ),
        (error) => error.code === 'idempotency_conflict',
    );
});

test('a later failed voice report cannot overwrite an already succeeded fact', async () => {
    const { gateway } = actionGateway();
    await gateway.application.actions.recordDeviceActionStatus(voiceReport({ occurredAt: '2026-08-03T00:02:00.000Z' }));
    await assert.rejects(
        () =>
            gateway.application.actions.recordDeviceActionStatus(
                voiceReport({
                    eventId: 'voice-failed-later',
                    operationId: 'voice-failed-later-operation',
                    status: 'failed',
                    occurredAt: '2026-08-03T00:03:00.000Z',
                    errorCode: 'device_busy',
                }),
            ),
        (error) => error.code === 'invalid_transition',
    );
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const action = await gateway.application.actions.find(command.commandId);
    assert.equal(action.status, 'succeeded');
});

test('a concurrent same-time device report cannot replace the first accepted fact', async () => {
    const { gateway } = actionGateway();
    await gateway.application.actions.recordDeviceActionStatus(voiceReport());
    await assert.rejects(
        () =>
            gateway.application.actions.recordDeviceActionStatus(
                voiceReport({
                    eventId: 'voice-same-time-conflict',
                    operationId: 'voice-same-time-operation',
                    action: 'snooze',
                    nextTriggerAt: '2026-08-03T00:11:00.000Z',
                }),
            ),
        (error) => error.code === 'invalid_transition',
    );
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const action = await gateway.application.actions.find(command.commandId);
    assert.equal(action.status, 'succeeded');
    assert.equal(action.result.nextTriggerAt, undefined);
});

test('concurrent first device reports converge on one same-time fact', async () => {
    const unitOfWork = new InMemoryImUnitOfWork();
    seedDevice(unitOfWork, 'device-fixture');
    const { gateway } = actionGateway({ unitOfWork });
    const reports = [
        voiceReport({ eventId: 'voice-concurrent-a', operationId: 'voice-concurrent-a-operation' }),
        voiceReport({
            eventId: 'voice-concurrent-b',
            operationId: 'voice-concurrent-b-operation',
            action: 'snooze',
            nextTriggerAt: '2026-08-03T00:11:00.000Z',
        }),
    ];
    const outcomes = await Promise.allSettled(
        reports.map((report) => gateway.application.actions.recordDeviceActionStatus(report)),
    );
    assert.equal(outcomes.filter((outcome) => outcome.status === 'fulfilled').length, 1);
    assert.equal(outcomes.filter((outcome) => outcome.status === 'rejected').length, 1);
    const rejected = outcomes.find((outcome) => outcome.status === 'rejected');
    assert.equal(rejected.reason.code, 'invalid_transition');
});

test('an older device report cannot roll back a newer report', async () => {
    const { gateway } = actionGateway();
    await gateway.application.actions.recordDeviceActionStatus(
        voiceReport({ eventId: 'voice-new', occurredAt: '2026-08-03T00:02:00.000Z' }),
    );
    await gateway.application.actions.recordDeviceActionStatus(
        voiceReport({
            eventId: 'voice-old',
            operationId: 'voice-old-operation',
            occurredAt: '2026-08-03T00:01:00.000Z',
            status: 'failed',
        }),
    );
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const action = await gateway.application.actions.find(command.commandId);
    assert.equal(action.status, 'succeeded');
});

test('resolveActionWindow accepts only the matching active strong-reminder window', async () => {
    const { gateway, clock } = buildGateway();
    await pendingStrongDelivery(gateway);

    assert.equal(
        await gateway.application.actions.resolveActionWindow('device-fixture', 'trigger-fixture'),
        clock.addMinutes(clock.now(), 10),
    );
    const error = await expectRejected(
        () => gateway.application.actions.resolveActionWindow('device-fixture', 'trigger-other'),
        'A non-existent action window was resolved',
    );
    assert.equal(error.code, 'action_expired');
});

test('concurrently triggering the same token is idempotent and dispatches once', async () => {
    const { gateway, stream } = actionGateway();
    const { token } = await prepareAction(gateway);

    const [first, second] = await Promise.all([
        gateway.application.actionUi.execute({ token, action: 'acknowledge' }),
        gateway.application.actionUi.execute({ token, action: 'acknowledge' }),
    ]);

    assert.equal(first.commandId, second.commandId);
    assert.equal(stream.commands.length, 1);
    assert.equal(stream.commands[0].commandId, first.commandId);
});

test('an idempotent replay is rejected after its binding is revoked', async () => {
    const { gateway, stream } = actionGateway();
    const { deliveryId, token } = await prepareAction(gateway);
    const first = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const details = await gateway.application.deliveries.find(deliveryId);
    await gateway.application.bindings.revoke(details.delivery.bindingId);

    const error = await expectRejected(
        () => gateway.application.actionUi.execute({ token, action: 'acknowledge' }),
        'A revoked binding replayed an existing action',
    );

    assert.equal(error.code, 'action_expired');
    assert.equal(stream.commands.length, 1);
    assert.equal(stream.commands[0].commandId, first.commandId);
});

test('Last-Event-ID does not acknowledge an unconfirmed action command', async () => {
    const { gateway } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });

    const replay = await gateway.application.actions.replayPending(
        command.deviceId,
        command.reminderTriggerId,
        command.commandId,
    );

    assert.equal(replay.length, 1);
    assert.equal(replay[0].commandId, command.commandId);
    assert.equal(replay[0].operationId, command.operationId);
});

test('replayPending with an unknown cursor replays the whole unconfirmed window', async () => {
    const { gateway } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });

    const replay = await gateway.application.actions.replayPending(
        command.deviceId,
        command.reminderTriggerId,
        'action-cursor-unknown',
    );

    assert.equal(replay.length, 1);
    assert.equal(replay[0].commandId, command.commandId);
});

test('rebinding a device sends new notifications only to its current identity', async () => {
    const { gateway } = actionGateway();
    const first = await bindFixtureUser(gateway, { externalUserId: 'fixture-open-id-1' });
    const second = await bindFixtureUser(gateway, { externalUserId: 'fixture-open-id-2' });
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    assert.equal(submission.deliveries.length, 1);
    assert.equal(submission.deliveries[0].bindingId, second.binding.id);
    assert.equal(
        (await gateway.application.bindings.list('user-fixture')).some((binding) => binding.id === first.binding.id),
        false,
    );
});

test('markProcessing validates command scope and is idempotent once processing', async () => {
    const { gateway } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });

    await gateway.application.actions.markProcessing(command.commandId, command.deviceId, command.reminderTriggerId);
    await gateway.application.actions.markProcessing(command.commandId, command.deviceId, command.reminderTriggerId);
    assert.equal((await gateway.application.actions.find(command.commandId)).status, 'processing');

    const error = await expectRejected(
        () => gateway.application.actions.markProcessing(command.commandId, 'device-other', command.reminderTriggerId),
        'A command entered processing for the wrong device',
    );
    assert.equal(error.code, 'invalid_transition');
});

test('reusing a token for a different action type is rejected', async () => {
    const { gateway } = actionGateway();
    const { token } = await prepareAction(gateway);
    await gateway.application.actionUi.execute({ token, action: 'acknowledge' });

    const error = await expectRejected(
        () => gateway.application.actionUi.execute({ token, action: 'snooze', params: { minutes: 10 } }),
        'A token reused for another action type was not rejected',
    );
    assert.equal(error.code, 'action_not_found');
});

test('an acknowledge action rejects action params', async () => {
    const { gateway } = actionGateway();
    const { token } = await prepareAction(gateway);

    const error = await expectRejected(
        () => gateway.application.actionUi.execute({ token, action: 'acknowledge', params: { minutes: 10 } }),
        'An acknowledge action with params was not rejected',
    );
    assert.equal(error.code, 'invalid_transition');
});

test('snooze params must be a positive integer minutes', async () => {
    for (const params of [undefined, { minutes: 0 }, { minutes: 1.5 }]) {
        const { gateway } = buildGateway();
        const { token } = await prepareAction(gateway);
        const input = { token, action: 'snooze', ...(params === undefined ? {} : { params }) };

        const error = await expectRejected(
            () => gateway.application.actionUi.execute(input),
            'Snooze with invalid params was not rejected',
        );
        assert.equal(error.code, 'invalid_transition');
    }
});

test('snooze params must match the server-approved option across replays', async () => {
    const { gateway } = actionGateway();
    const { token } = await prepareAction(gateway);

    const unapproved = await expectRejected(
        () => gateway.application.actionUi.execute({ token, action: 'snooze', params: { minutes: 11 } }),
        'Snooze accepted params that were not rendered by the server',
    );
    assert.equal(unapproved.code, 'action_expired');

    await gateway.application.actionUi.execute({ token, action: 'snooze', params: { minutes: 10 } });
    const changedReplay = await expectRejected(
        () => gateway.application.actionUi.execute({ token, action: 'snooze', params: { minutes: 11 } }),
        'Snooze replay silently changed the approved params',
    );
    assert.equal(changedReplay.code, 'action_not_found');
});

test('malformed persisted action options are rejected instead of becoming executable actions', async () => {
    for (const actions of [
        [{ kind: 'command', type: 'acknowledge', label: '' }],
        [{ kind: 'command', type: 'snooze', label: '推迟', params: { minutes: 0 } }],
        [{ kind: 'command', type: 'snooze', label: '推\n迟', params: { minutes: 10 } }],
        [{ kind: 'command', type: 'snooze', label: '推\u202E迟', params: { minutes: 10 } }],
        [{ kind: 'command', type: 'snooze', label: '推'.repeat(129), params: { minutes: 10 } }],
        [{ kind: 'command', type: 'snooze', label: '推迟', params: { minutes: 1441 } }],
    ]) {
        const clock = new FixedClock();
        const unitOfWork = new InMemoryImUnitOfWork();
        const gateway = createImGateway({
            unitOfWork,
            ...mockImGatewayPorts('device-fixture', clock),
        });
        seedDevice(unitOfWork, 'device-fixture');
        const deliveryId = await pendingStrongDelivery(gateway);
        await unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findById(deliveryId);
            await tx.deliveries.save({
                ...delivery,
                semanticPayload: { ...delivery.semanticPayload, actions },
            });
        });
        const error = await expectRejected(
            () => gateway.application.actionUi.issue(deliveryId),
            'Malformed persisted action options were accepted',
        );
        assert.equal(error.code, 'action_expired');
    }
});

test('a snooze success without nextTriggerAt is rejected', async () => {
    const { gateway, clock } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'snooze', params: { minutes: 10 } });

    const error = await expectRejected(
        () =>
            gateway.application.actions.recordResult(
                command.commandId,
                'device-fixture',
                succeededResult(command, clock.now()),
            ),
        'A snooze success without nextTriggerAt was not rejected',
    );
    assert.equal(error.code, 'invalid_transition');
});

test('an acknowledge success with nextTriggerAt is rejected', async () => {
    const { gateway, clock } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });

    const error = await expectRejected(
        () =>
            gateway.application.actions.recordResult(
                command.commandId,
                'device-fixture',
                succeededResult(command, clock.now(), { nextTriggerAt: '2026-08-03T00:20:00.000Z' }),
            ),
        'An acknowledge success with nextTriggerAt was not rejected',
    );
    assert.equal(error.code, 'invalid_transition');
});

test('a snooze success with nextTriggerAt records the result and closes the stream', async () => {
    const { gateway, clock, stream } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'snooze', params: { minutes: 10 } });

    const updated = await gateway.application.actions.recordResult(
        command.commandId,
        'device-fixture',
        succeededResult(command, clock.now(), { nextTriggerAt: '2026-08-03T00:20:00.000Z' }),
    );

    assert.equal(updated.status, 'succeeded');
    assert.equal(updated.result.status, 'succeeded');
    assert.equal(updated.result.nextTriggerAt, '2026-08-03T00:20:00.000Z');
    assert.equal(stream.closed.includes(command.commandId), true);
});

test('a result that does not match the command scope is rejected', async () => {
    const { gateway, clock } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });

    const wrongDevice = await expectRejected(
        () =>
            gateway.application.actions.recordResult(
                command.commandId,
                'device-other',
                succeededResult(command, clock.now()),
            ),
        'A result from the wrong device was not rejected',
    );
    assert.equal(wrongDevice.code, 'invalid_transition');
    const wrongOperation = await expectRejected(
        () =>
            gateway.application.actions.recordResult(
                command.commandId,
                'device-fixture',
                succeededResult(command, clock.now(), { operationId: 'operation-999' }),
            ),
        'A result with the wrong operation was not rejected',
    );
    assert.equal(wrongOperation.code, 'invalid_transition');
});

test('a result for an unknown command is rejected', async () => {
    const { gateway, clock } = actionGateway();

    const error = await expectRejected(
        () =>
            gateway.application.actions.recordResult('action-missing', 'device-fixture', {
                schemaVersion: '1',
                operationId: 'operation-missing',
                reminderTriggerId: 'trigger-fixture',
                status: 'failed',
                occurredAt: clock.now(),
            }),
        'A result for an unknown command was accepted',
    );
    assert.equal(error.code, 'action_not_found');
});

test('a terminal action result cannot be overwritten', async () => {
    const { gateway, clock } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const succeeded = await gateway.application.actions.recordResult(
        command.commandId,
        'device-fixture',
        succeededResult(command, clock.now()),
    );
    assert.equal(succeeded.status, 'succeeded');

    const error = await expectRejected(
        () =>
            gateway.application.actions.recordResult(
                command.commandId,
                'device-fixture',
                succeededResult(command, clock.now(), { status: 'failed' }),
            ),
        'A terminal action result was overwritten',
    );
    assert.equal(error.code, 'invalid_transition');
});

test('a retryable failure returns the action to pending and re-dispatches', async () => {
    const { gateway, clock, stream } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });

    const updated = await gateway.application.actions.recordResult(
        command.commandId,
        'device-fixture',
        succeededResult(command, clock.now(), { status: 'retryable_failed' }),
    );

    assert.equal(updated.status, 'pending');
    assert.equal(stream.commands.length, 2);
    assert.equal(stream.commands[1].commandId, command.commandId);
    const found = await gateway.application.actions.find(command.commandId);
    assert.equal(found.status, 'dispatched');
});

test('expireDue expires and closes actions past their deadline', async () => {
    const { gateway, clock, stream } = actionGateway();
    const { token } = await prepareAction(gateway);
    const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    clock.advanceMinutes(11);

    const count = await gateway.application.actions.expireDue();

    assert.equal(count, 1);
    const found = await gateway.application.actions.find(command.commandId);
    assert.equal(found.status, 'expired');
    assert.equal(stream.closed.includes(command.commandId), true);
});
