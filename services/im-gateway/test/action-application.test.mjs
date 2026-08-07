import { test } from 'node:test';
import assert from 'node:assert/strict';

import { createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import {
    bindFixtureUser,
    buildGateway,
    expectRejected,
    pendingStrongDelivery,
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

test('triggering the same token twice is idempotent and dispatches once', async () => {
    const { gateway, stream } = actionGateway();
    const { token } = await prepareAction(gateway);

    const first = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
    const second = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });

    assert.equal(first.commandId, second.commandId);
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

test('Last-Event-ID does not exclude earlier unconfirmed commands in the same window', async () => {
    const { gateway } = actionGateway();
    await bindFixtureUser(gateway, { externalUserId: 'fixture-open-id-1' });
    await bindFixtureUser(gateway, { externalUserId: 'fixture-open-id-2' });
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    assert.equal(submission.deliveries.length, 2);
    const commands = [];
    for (const delivery of submission.deliveries) {
        const token = await gateway.application.actionUi.issue(delivery.deliveryId);
        commands.push(await gateway.application.actionUi.execute({ token, action: 'acknowledge' }));
    }

    const replay = await gateway.application.actions.replayPending(
        commands[1].deviceId,
        commands[1].reminderTriggerId,
        commands[1].commandId,
    );

    assert.deepEqual(
        replay.map((command) => command.commandId),
        commands.map((command) => command.commandId),
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
