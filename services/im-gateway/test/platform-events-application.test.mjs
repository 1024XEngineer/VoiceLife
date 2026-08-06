import { test } from 'node:test';
import assert from 'node:assert/strict';

import { createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import { expectRejected, pendingStrongDelivery, registerChannel } from './helpers.mjs';

/** 暴露内存仓储的入站事件行,便于断言状态机。 */
class ExposedUnitOfWork extends InMemoryImUnitOfWork {
    findInbound(eventId) {
        return [...this.inboundRows.values()].find((event) => event.id === eventId);
    }
}

/** 记录发布命令的动作流 Port。 */
function recordingActionStream() {
    const commands = [];
    return {
        commands,
        port: {
            publish: async (command) => {
                commands.push(command);
            },
            subscribe: async function* () {},
            close: async () => {},
        },
    };
}

/** 构造一条规范化平台入站事件。 */
function platformEvent(overrides = {}) {
    return {
        id: 'inbound-1',
        externalEventId: 'ext-1',
        platform: 'wechat_official',
        channelAccountId: 'channel-1',
        occurredAt: '2026-08-03T00:00:00.000Z',
        ...overrides,
    };
}

/** 创建强提醒投递并签发动作令牌。 */
async function prepareActionTrigger(gateway) {
    const deliveryId = await pendingStrongDelivery(gateway);
    const token = await gateway.application.actionUi.issue(deliveryId);
    return { deliveryId, token };
}

test('a message.received event is recorded and processed without dispatch', async () => {
    const clock = new FixedClock();
    const uow = new ExposedUnitOfWork();
    const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: uow });
    const event = platformEvent({ type: 'message.received', payload: { text: 'hello' } });

    const result = await gateway.application.platformEvents.postEvent(event);

    assert.equal(result, undefined);
    const record = uow.findInbound(event.id);
    assert.equal(record.status, 'processed');
    assert.equal(record.eventType, 'message.received');
});

test('an action.triggered event executes the action and publishes a device command', async () => {
    const clock = new FixedClock();
    const stream = recordingActionStream();
    const gateway = createMockImGateway('device-fixture', clock, { actionStream: stream.port });
    const { deliveryId, token } = await prepareActionTrigger(gateway);
    const event = platformEvent({ type: 'action.triggered', payload: { token, action: 'acknowledge' } });

    const command = await gateway.application.platformEvents.postEvent(event);

    assert.equal(command.commandId, `action-ui:${deliveryId}`);
    assert.equal(command.action, 'acknowledge');
    assert.equal(command.deviceId, 'device-fixture');
    assert.equal(stream.commands.length, 1);
    assert.equal(stream.commands[0].commandId, command.commandId);
});

test('a duplicate action.triggered event is not dispatched twice', async () => {
    const clock = new FixedClock();
    const stream = recordingActionStream();
    const gateway = createMockImGateway('device-fixture', clock, { actionStream: stream.port });
    const { token } = await prepareActionTrigger(gateway);
    const event = platformEvent({ type: 'action.triggered', payload: { token, action: 'acknowledge' } });

    const first = await gateway.application.platformEvents.postEvent(event);
    const second = await gateway.application.platformEvents.postEvent(event);

    assert.equal(typeof first.commandId, 'string');
    assert.equal(second, undefined);
    assert.equal(stream.commands.length, 1);
});

test('an action triggered by the wrong identity fails and marks the event failed', async () => {
    const clock = new FixedClock();
    const uow = new ExposedUnitOfWork();
    const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: uow });
    const { token } = await prepareActionTrigger(gateway);
    const event = platformEvent({
        type: 'action.triggered',
        externalIdentityId: 'identity-other',
        payload: { token, action: 'acknowledge' },
    });

    const error = await expectRejected(
        () => gateway.application.platformEvents.postEvent(event),
        'Action triggered by the wrong identity was not rejected',
    );
    assert.equal(error.code, 'action_expired');
    const record = uow.findInbound(event.id);
    assert.equal(record.status, 'failed');
});

test('a binding.requested event confirms a pairing session into a binding', async () => {
    const clock = new FixedClock();
    const gateway = createMockImGateway('device-fixture', clock);
    const channel = await registerChannel(gateway);
    await gateway.application.pairing.create({ userId: 'user-fixture', deviceId: 'device-fixture' });
    const event = platformEvent({
        type: 'binding.requested',
        channelAccountId: channel.id,
        payload: { displayCode: '123456', externalUserId: 'open-bind' },
    });

    await gateway.application.platformEvents.postEvent(event);

    const bindings = await gateway.application.bindings.list('user-fixture');
    assert.equal(bindings.length, 1);
    assert.equal(bindings[0].status, 'active');
    assert.match(bindings[0].externalIdentityId, /^identity-/);
});

test('a duplicate binding.requested event does not confirm a second binding', async () => {
    const clock = new FixedClock();
    const gateway = createMockImGateway('device-fixture', clock);
    const channel = await registerChannel(gateway);
    await gateway.application.pairing.create({ userId: 'user-fixture', deviceId: 'device-fixture' });
    const event = platformEvent({
        type: 'binding.requested',
        channelAccountId: channel.id,
        payload: { displayCode: '123456', externalUserId: 'open-bind' },
    });

    await gateway.application.platformEvents.postEvent(event);
    await gateway.application.platformEvents.postEvent(event);

    const bindings = await gateway.application.bindings.list('user-fixture');
    assert.equal(bindings.length, 1);
});

test('a delivered receipt event advances the delivery to delivered', async () => {
    const clock = new FixedClock();
    const gateway = createMockImGateway('device-fixture', clock);
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    const details = await gateway.application.deliveries.find(deliveryId);
    const event = platformEvent({
        type: 'delivery.updated',
        channelAccountId: details.delivery.channelAccountId,
        payload: {
            externalEventId: 'rcpt-1',
            channelAccountId: details.delivery.channelAccountId,
            externalMessageId: details.delivery.externalMessageId,
            dedupeKey: 'rcpt-dedupe-1',
            stage: 'delivered',
            occurredAt: '2026-08-03T00:01:00.000Z',
        },
    });

    await gateway.application.platformEvents.postEvent(event);

    const after = await gateway.application.deliveries.find(deliveryId);
    assert.equal(after.delivery.status, 'delivered');
    assert.equal(after.receipts.length, 1);
    assert.equal(after.receipts[0].stage, 'delivered');
});

test('a receipt whose envelope channel mismatches is rejected', async () => {
    const clock = new FixedClock();
    const uow = new ExposedUnitOfWork();
    const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: uow });
    await pendingStrongDelivery(gateway);
    const event = platformEvent({
        type: 'delivery.updated',
        channelAccountId: 'channel-other',
        payload: {
            externalEventId: 'rcpt-x',
            channelAccountId: 'channel-real',
            externalMessageId: 'message-x',
            dedupeKey: 'rcpt-dedupe-x',
            stage: 'delivered',
            occurredAt: '2026-08-03T00:01:00.000Z',
        },
    });

    const error = await expectRejected(
        () => gateway.application.platformEvents.postEvent(event),
        'A mismatched receipt envelope was not rejected',
    );
    assert.equal(error.code, 'invalid_transition');
    const record = uow.findInbound(event.id);
    assert.equal(record.status, 'failed');
});

test('a receipt for an unknown message is rejected', async () => {
    const clock = new FixedClock();
    const uow = new ExposedUnitOfWork();
    const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: uow });
    const deliveryId = await pendingStrongDelivery(gateway);
    const details = await gateway.application.deliveries.find(deliveryId);
    const event = platformEvent({
        type: 'delivery.updated',
        channelAccountId: details.delivery.channelAccountId,
        payload: {
            externalEventId: 'rcpt-y',
            channelAccountId: details.delivery.channelAccountId,
            externalMessageId: 'unknown-message',
            dedupeKey: 'rcpt-dedupe-y',
            stage: 'delivered',
            occurredAt: '2026-08-03T00:01:00.000Z',
        },
    });

    const error = await expectRejected(
        () => gateway.application.platformEvents.postEvent(event),
        'A receipt for an unknown message was not rejected',
    );
    assert.equal(error.code, 'delivery_not_found');
    const record = uow.findInbound(event.id);
    assert.equal(record.status, 'failed');
});
