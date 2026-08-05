import { test } from 'node:test';
import assert from 'node:assert/strict';

import { createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import { buildGateway, expectGatewayError, pendingStrongDelivery } from './helpers.mjs';

/** 暴露内存仓储的 outbox 行,便于断言重试请求事件。 */
class ExposedUnitOfWork extends InMemoryImUnitOfWork {
    outboxEvents() {
        return [...this.outboxRows];
    }
}

/** 按脚本逐个返回发送结果(或抛异常)的可编程 IM 渠道。 */
function gatewayWithChannel(script, overrides = {}) {
    const clock = new FixedClock();
    const imChannel = {
        send: async () => {
            const step = script.shift();
            if (step === undefined) return { accepted: true, platformMessageId: 'platform-1' };
            return step;
        },
    };
    const gateway = createMockImGateway('device-fixture', clock, { imChannel, ...overrides });
    return { gateway, clock };
}

/** 派发并推进到 delivered 状态。 */
async function deliverPendingDelivery(gateway, clock) {
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    const details = await gateway.application.deliveries.find(deliveryId);
    await gateway.application.receipts.record({
        externalEventId: 'rcpt-1',
        channelAccountId: details.delivery.channelAccountId,
        externalMessageId: details.delivery.externalMessageId,
        dedupeKey: 'rcpt-dedupe-1',
        stage: 'delivered',
        occurredAt: clock.now(),
    });
    return deliveryId;
}

test('find returns the delivery aggregate with attempts and receipts', async () => {
    const { gateway, clock } = gatewayWithChannel([{ accepted: true, platformMessageId: 'platform-1' }]);
    const deliveryId = await deliverPendingDelivery(gateway, clock);

    const details = await gateway.application.deliveries.find(deliveryId);

    assert.equal(details.delivery.id, deliveryId);
    assert.equal(details.delivery.status, 'delivered');
    assert.equal(details.attempts.length, 1);
    assert.equal(details.receipts.length, 1);
});

test('find of an unknown delivery returns undefined', async () => {
    const { gateway } = buildGateway();

    const details = await gateway.application.deliveries.find('delivery-missing');

    assert.equal(details, undefined);
});

test('retrying a dead-letter delivery restores it to pending and requests a retry', async () => {
    const uow = new ExposedUnitOfWork();
    // 非可重试拒绝(permanent_failed)必须先经 markDeadLetter 进入死信队列,才可手动重试。
    const { gateway, clock } = gatewayWithChannel([{ accepted: false, retryable: false, errorCode: 'blocked' }], {
        unitOfWork: uow,
    });
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    await gateway.application.deliveryDispatch.markDeadLetter(deliveryId);

    const pending = await gateway.application.deliveries.retryDeadLetter(deliveryId);

    assert.equal(pending.status, 'pending');
    const events = uow.outboxEvents().filter((event) => event.eventType === 'im.delivery.retry-requested');
    assert.equal(events.length, 1);
    assert.equal(events[0].aggregateId, deliveryId);
    assert.deepEqual(events[0].payload, { deliveryId });
    assert.equal(events[0].status, 'pending');
    assert.equal(events[0].attempts, 0);
    assert.equal(events[0].availableAt, clock.now());
});

test('retrying a permanent_failed delivery restores it to pending and requests a retry', async () => {
    const uow = new ExposedUnitOfWork();
    const { gateway, clock } = gatewayWithChannel([{ accepted: false, retryable: false, errorCode: 'blocked' }], {
        unitOfWork: uow,
    });
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    const pending = await gateway.application.deliveries.retryDeadLetter(deliveryId);

    assert.equal(pending.status, 'pending');
    const events = uow.outboxEvents().filter((event) => event.eventType === 'im.delivery.retry-requested');
    assert.equal(events.length, 1);
    assert.equal(events[0].aggregateId, deliveryId);
    assert.equal(events[0].availableAt, clock.now());
});

test('retrying a pending delivery is rejected', async () => {
    const { gateway } = buildGateway();
    const deliveryId = await pendingStrongDelivery(gateway);

    await expectGatewayError(
        () => gateway.application.deliveries.retryDeadLetter(deliveryId),
        'invalid_transition',
        'Retrying a pending delivery was not rejected',
    );
});

test('retrying an accepted delivery is rejected', async () => {
    const { gateway } = gatewayWithChannel([{ accepted: true, platformMessageId: 'platform-1' }]);
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    await expectGatewayError(
        () => gateway.application.deliveries.retryDeadLetter(deliveryId),
        'invalid_transition',
        'Retrying an accepted delivery was not rejected',
    );
});

test('retrying a delivered delivery is rejected', async () => {
    const { gateway, clock } = gatewayWithChannel([{ accepted: true, platformMessageId: 'platform-1' }]);
    const deliveryId = await deliverPendingDelivery(gateway, clock);

    await expectGatewayError(
        () => gateway.application.deliveries.retryDeadLetter(deliveryId),
        'invalid_transition',
        'Retrying a delivered delivery was not rejected',
    );
});

test('retrying a retryable_failed delivery is rejected', async () => {
    const { gateway } = gatewayWithChannel([{ accepted: false, retryable: true, errorCode: 'busy' }]);
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);

    await expectGatewayError(
        () => gateway.application.deliveries.retryDeadLetter(deliveryId),
        'invalid_transition',
        'Retrying a retryable_failed delivery was not rejected',
    );
});

test('retrying an unknown delivery is rejected', async () => {
    const { gateway } = buildGateway();

    await expectGatewayError(
        () => gateway.application.deliveries.retryDeadLetter('delivery-missing'),
        'delivery_not_found',
        'Retrying an unknown delivery was not rejected',
    );
});
