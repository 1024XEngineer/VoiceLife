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

test('dispatch records a permanent failure when the delivery target binding disappears', async () => {
    const clock = new FixedClock();
    const uow = new ExposedUnitOfWork();
    const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: uow });
    await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    uow.deleteBinding(submission.deliveries[0].bindingId);

    const updated = await gateway.application.deliveryDispatch.dispatch(submission.deliveries[0].deliveryId);

    assert.equal(updated.status, 'permanent_failed');
    assert.equal(updated.lastErrorCode, 'delivery_target_unavailable');
    const details = await gateway.application.deliveries.find(updated.id);
    assert.equal(details.attempts.length, 1);
    assert.equal(details.attempts[0].status, 'permanent_failed');
});

test('dispatch records a permanent failure when the channel account is disabled after submission', async () => {
    const { gateway, sent } = gatewayWithChannel([]);
    const { channel } = await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    await gateway.application.channels.disable(channel.id);

    const updated = await gateway.application.deliveryDispatch.dispatch(submission.deliveries[0].deliveryId);

    assert.equal(updated.status, 'permanent_failed');
    assert.equal(updated.lastErrorCode, 'delivery_target_unavailable');
    assert.equal(sent.length, 0);
});

test('dispatch records a permanent failure when the binding is terminated after submission', async () => {
    const { gateway, sent } = gatewayWithChannel([]);
    const { binding } = await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    await gateway.application.bindings.revoke(binding.id);

    const updated = await gateway.application.deliveryDispatch.dispatch(submission.deliveries[0].deliveryId);

    assert.equal(updated.status, 'permanent_failed');
    assert.equal(updated.lastErrorCode, 'delivery_target_unavailable');
    assert.equal(sent.length, 0);
});

test('dispatch records a permanent failure when the external identity is revoked after submission', async () => {
    const uow = new ExposedUnitOfWork();
    const { gateway, sent } = gatewayWithChannel([], { unitOfWork: uow });
    const { binding } = await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    uow.setIdentityStatus(binding.externalIdentityId, 'revoked');

    const updated = await gateway.application.deliveryDispatch.dispatch(submission.deliveries[0].deliveryId);

    assert.equal(updated.status, 'permanent_failed');
    assert.equal(updated.lastErrorCode, 'delivery_target_unavailable');
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

test('automatic delivery retries use bounded backoff and become permanent after the fifth attempt', async () => {
    const uow = new ExposedUnitOfWork();
    const failures = Array.from({ length: 5 }, () => ({
        accepted: false,
        retryable: true,
        errorCode: 'busy',
    }));
    const { gateway, clock } = gatewayWithChannel(failures, { unitOfWork: uow });
    const deliveryId = await pendingStrongDelivery(gateway);

    let updated;
    for (let attempt = 0; attempt < 5; attempt += 1) {
        updated = await gateway.application.deliveryDispatch.dispatch(deliveryId);
    }

    assert.equal(updated.status, 'permanent_failed');
    assert.equal(updated.lastErrorCode, 'delivery_retry_exhausted');
    const details = await gateway.application.deliveries.find(deliveryId);
    assert.equal(details.attempts.length, 5);
    assert.equal(details.attempts[4].status, 'retryable_failed');
    const retries = uow.outboxEvents().filter((event) => event.eventType === 'im.delivery.retry-scheduled');
    assert.equal(retries.length, 4);
    assert.deepEqual(
        retries.map((event) => event.availableAt),
        [1, 2, 4, 8].map((minutes) => clock.addMinutes(clock.now(), minutes)),
    );
});

test('a channel send exception is treated as a retryable failure', async () => {
    const { gateway } = gatewayWithChannel(['throw']);
    const deliveryId = await pendingStrongDelivery(gateway);

    const updated = await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(updated.status, 'retryable_failed');
    assert.equal(updated.lastErrorCode, 'channel_send_exception');
});

test('a pre-send failure becomes retryable_failed with one retry outbox, then recovers', async () => {
    const clock = new FixedClock();
    const uow = new ExposedUnitOfWork();
    let failRender = true;
    const gateway = createMockImGateway('device-fixture', clock, {
        unitOfWork: uow,
        deliveryRenderer: {
            render: async () => {
                if (failRender) throw new Error('render boom');
                return { ok: true };
            },
        },
    });
    const deliveryId = await pendingStrongDelivery(gateway);

    const failed = await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(failed.status, 'retryable_failed');
    assert.equal(failed.lastErrorCode, 'pre_send_exception');
    assert.equal(failed.claimedAt, undefined);
    assert.equal(failed.claimToken, undefined);
    const failedDetails = await gateway.application.deliveries.find(deliveryId);
    assert.equal(failedDetails.attempts.length, 1);
    assert.equal(failedDetails.attempts[0].status, 'retryable_failed');
    assert.equal(failedDetails.attempts[0].errorCode, 'pre_send_exception');
    const retries = uow.outboxEvents().filter((event) => event.eventType === 'im.delivery.retry-scheduled');
    assert.equal(retries.length, 1);

    // 修好渲染器后重派发：retryable_failed 可重领并成功
    failRender = false;
    const recovered = await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(recovered.status, 'accepted');
    assert.equal(recovered.lastErrorCode, undefined);
    const recoveredDetails = await gateway.application.deliveries.find(deliveryId);
    assert.equal(recoveredDetails.attempts.length, 2);
    assert.equal(recoveredDetails.attempts[1].status, 'accepted');
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
