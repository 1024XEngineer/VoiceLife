import { test } from 'node:test';
import assert from 'node:assert/strict';

import { createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import {
    bindFixtureUser,
    buildGateway,
    expectGatewayError,
    pendingStrongDelivery,
    strongIntent,
    weakIntent,
} from './helpers.mjs';

/** 暴露内存仓储的 outbox 与绑定行,便于断言内部状态。 */
class ExposedUnitOfWork extends InMemoryImUnitOfWork {
    outboxEvents() {
        return [...this.outboxRows];
    }

    deleteBinding(bindingId) {
        this.bindingRows.delete(bindingId);
    }

    setIdentityStatus(identityId, status) {
        const identity = this.identityRows.get(identityId);
        this.identityRows.set(identityId, { ...identity, status });
    }
}

/** 按脚本逐个返回发送结果(或抛异常)的可编程 IM 渠道。 */
function gatewayWithChannel(script, overrides = {}) {
    const clock = new FixedClock();
    const sent = [];
    const imChannel = {
        send: async (message) => {
            sent.push(message);
            const step = script.shift();
            if (step === undefined) return { accepted: true, platformMessageId: `platform-${sent.length}` };
            if (step === 'throw') throw new Error('channel offline');
            return step;
        },
    };
    const gateway = createMockImGateway('device-fixture', clock, { imChannel, ...overrides });
    return { gateway, clock, sent };
}

test('dispatch of a pending delivery sends and records an accepted attempt', async () => {
    const { gateway, sent } = gatewayWithChannel([{ accepted: true, platformMessageId: 'platform-1' }]);
    const deliveryId = await pendingStrongDelivery(gateway);

    const updated = await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(updated.status, 'accepted');
    assert.equal(updated.externalMessageId, 'platform-1');
    const details = await gateway.application.deliveries.find(deliveryId);
    assert.equal(details.attempts.length, 1);
    assert.equal(details.attempts[0].attemptNo, 1);
    assert.equal(details.attempts[0].status, 'accepted');
    assert.equal(details.attempts[0].platformMessageId, 'platform-1');
    assert.equal(sent.length, 1);
});

test('concurrent dispatch of the same delivery sends exactly once', async () => {
    const { gateway, sent } = gatewayWithChannel([{ accepted: true, platformMessageId: 'platform-1' }]);
    const deliveryId = await pendingStrongDelivery(gateway);

    const results = await Promise.allSettled([
        gateway.application.deliveryDispatch.dispatch(deliveryId),
        gateway.application.deliveryDispatch.dispatch(deliveryId),
    ]);

    assert.equal(sent.length, 1);
    const fulfilled = results.filter((result) => result.status === 'fulfilled');
    const rejected = results.filter((result) => result.status === 'rejected');
    assert.equal(fulfilled.length, 1);
    assert.equal(fulfilled[0].value.status, 'accepted');
    assert.equal(rejected.length, 1);
    assert.match(rejected[0].reason.message, /Only pending or retryable deliveries can be dispatched/);
    const details = await gateway.application.deliveries.find(deliveryId);
    assert.equal(details.attempts.length, 1);
});

test('dispatch of an unknown delivery is rejected', async () => {
    const { gateway } = buildGateway();

    await expectGatewayError(
        () => gateway.application.deliveryDispatch.dispatch('delivery-missing'),
        'delivery_not_found',
        'Dispatch of an unknown delivery was not rejected',
    );
});

test('dispatch of an already accepted delivery is rejected', async () => {
    const { gateway } = buildGateway();
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    await expectGatewayError(
        () => gateway.application.deliveryDispatch.dispatch(deliveryId),
        'invalid_transition',
        'Dispatch of an accepted delivery was not rejected',
    );
});

test('dispatch of a dead-letter delivery is rejected', async () => {
    const { gateway } = gatewayWithChannel([{ accepted: false, retryable: true, errorCode: 'busy' }]);
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    await gateway.application.deliveryDispatch.markDeadLetter(deliveryId);

    await expectGatewayError(
        () => gateway.application.deliveryDispatch.dispatch(deliveryId),
        'invalid_transition',
        'Dispatch of a dead-letter delivery was not rejected',
    );
});

test('dispatch is rejected when the delivery target binding is missing', async () => {
    const clock = new FixedClock();
    const uow = new ExposedUnitOfWork();
    const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: uow });
    await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    uow.deleteBinding(submission.deliveries[0].bindingId);

    await expectGatewayError(
        () => gateway.application.deliveryDispatch.dispatch(submission.deliveries[0].deliveryId),
        'binding_not_found',
        'Dispatch with a missing binding was not rejected',
    );
});

test('dispatch rejects a delivery whose channel account was disabled after submission', async () => {
    const { gateway, sent } = gatewayWithChannel([]);
    const { channel } = await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    await gateway.application.channels.disable(channel.id);

    await expectGatewayError(
        () => gateway.application.deliveryDispatch.dispatch(submission.deliveries[0].deliveryId),
        'binding_not_found',
        'Dispatch used a disabled channel account',
    );
    assert.equal(sent.length, 0);
});

test('dispatch rejects a delivery whose binding was terminated after submission', async () => {
    const { gateway, sent } = gatewayWithChannel([]);
    const { binding } = await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    await gateway.application.bindings.revoke(binding.id);

    await expectGatewayError(
        () => gateway.application.deliveryDispatch.dispatch(submission.deliveries[0].deliveryId),
        'binding_not_found',
        'Dispatch used a terminated binding',
    );
    assert.equal(sent.length, 0);
});

test('dispatch rejects a delivery whose external identity was revoked after submission', async () => {
    const uow = new ExposedUnitOfWork();
    const { gateway, sent } = gatewayWithChannel([], { unitOfWork: uow });
    const { binding } = await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    uow.setIdentityStatus(binding.externalIdentityId, 'revoked');

    await expectGatewayError(
        () => gateway.application.deliveryDispatch.dispatch(submission.deliveries[0].deliveryId),
        'binding_not_found',
        'Dispatch used a revoked external identity',
    );
    assert.equal(sent.length, 0);
});

test('a permanent platform rejection marks the delivery permanent_failed', async () => {
    const { gateway } = gatewayWithChannel([{ accepted: false, retryable: false, errorCode: 'blocked' }]);
    const deliveryId = await pendingStrongDelivery(gateway);

    const updated = await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(updated.status, 'permanent_failed');
    assert.equal(updated.lastErrorCode, 'blocked');
    assert.equal(updated.externalMessageId, undefined);
    const details = await gateway.application.deliveries.find(deliveryId);
    assert.equal(details.attempts[0].status, 'permanent_failed');
    assert.equal(details.attempts[0].errorCode, 'blocked');
});

test('a retryable platform rejection schedules a retry via the outbox', async () => {
    const uow = new ExposedUnitOfWork();
    const { gateway, clock } = gatewayWithChannel([{ accepted: false, retryable: true, errorCode: 'busy' }], {
        unitOfWork: uow,
    });
    const deliveryId = await pendingStrongDelivery(gateway);

    const updated = await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(updated.status, 'retryable_failed');
    assert.equal(updated.lastErrorCode, 'busy');
    const events = uow.outboxEvents().filter((event) => event.eventType === 'im.delivery.retry-scheduled');
    assert.equal(events.length, 1);
    assert.equal(events[0].eventType, 'im.delivery.retry-scheduled');
    assert.equal(events[0].aggregateId, deliveryId);
    assert.equal(events[0].status, 'pending');
    assert.equal(events[0].attempts, 1);
    assert.equal(events[0].availableAt, clock.addMinutes(clock.now(), 1));
});

test('a channel send exception is treated as a retryable failure', async () => {
    const { gateway } = gatewayWithChannel(['throw']);
    const deliveryId = await pendingStrongDelivery(gateway);

    const updated = await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(updated.status, 'retryable_failed');
    assert.equal(updated.lastErrorCode, 'channel_send_exception');
});

test('re-dispatching a retryable delivery resets it and records a second attempt', async () => {
    const { gateway } = gatewayWithChannel([
        { accepted: false, retryable: true, errorCode: 'busy' },
        { accepted: true, platformMessageId: 'platform-2' },
    ]);
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    const updated = await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(updated.status, 'accepted');
    assert.equal(updated.externalMessageId, 'platform-2');
    assert.equal(updated.lastErrorCode, undefined);
    const details = await gateway.application.deliveries.find(deliveryId);
    assert.equal(details.attempts.length, 2);
    assert.equal(details.attempts[1].attemptNo, 2);
    assert.equal(details.attempts[1].status, 'accepted');
});

test('dispatch of a strong delivery passes an action token to the renderer', async () => {
    const clock = new FixedClock();
    const rendered = [];
    const gateway = createMockImGateway('device-fixture', clock, {
        deliveryRenderer: {
            render: async (delivery, _account, _capabilities, context) => {
                rendered.push({ deliveryId: delivery.id, actionToken: context.actionToken });
                return { ok: true };
            },
        },
    });
    const deliveryId = await pendingStrongDelivery(gateway);

    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(rendered.length, 1);
    assert.equal(typeof rendered[0].actionToken, 'string');
});

test('dispatch without an action window renders without an action token', async () => {
    const clock = new FixedClock();
    const rendered = [];
    const gateway = createMockImGateway('device-fixture', clock, {
        deliveryRenderer: {
            render: async (delivery, _account, _capabilities, context) => {
                rendered.push({ deliveryId: delivery.id, actionToken: context.actionToken });
                return { ok: true };
            },
        },
    });
    await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(weakIntent());
    const deliveryId = submission.deliveries[0].deliveryId;

    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(rendered.length, 1);
    assert.equal(rendered[0].actionToken, undefined);
});

test('marking a retryable failure as dead letter transitions the delivery', async () => {
    const { gateway } = gatewayWithChannel([{ accepted: false, retryable: true, errorCode: 'busy' }]);
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    const deadLetter = await gateway.application.deliveryDispatch.markDeadLetter(deliveryId);

    assert.equal(deadLetter.status, 'dead_letter');
});

test('marking a permanent failure as dead letter transitions the delivery', async () => {
    const { gateway } = gatewayWithChannel([{ accepted: false, retryable: false, errorCode: 'blocked' }]);
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    const deadLetter = await gateway.application.deliveryDispatch.markDeadLetter(deliveryId);

    assert.equal(deadLetter.status, 'dead_letter');
});

test('marking a pending delivery as dead letter is rejected', async () => {
    const { gateway } = buildGateway();
    const deliveryId = await pendingStrongDelivery(gateway);

    await expectGatewayError(
        () => gateway.application.deliveryDispatch.markDeadLetter(deliveryId),
        'invalid_transition',
        'Marking a pending delivery as dead letter was not rejected',
    );
});

test('marking an unknown delivery as dead letter is rejected', async () => {
    const { gateway } = buildGateway();

    await expectGatewayError(
        () => gateway.application.deliveryDispatch.markDeadLetter('delivery-missing'),
        'delivery_not_found',
        'Marking an unknown delivery as dead letter was not rejected',
    );
});
