import { test } from 'node:test';
import assert from 'node:assert/strict';

import { buildGateway, expectRejected, pendingStrongDelivery } from './helpers.mjs';

/** 提交强提醒并派发到已接受,返回回执所需的投递上下文。 */
async function acceptedDelivery(gateway) {
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    const details = await gateway.application.deliveries.find(deliveryId);
    return {
        deliveryId,
        channelAccountId: details.delivery.channelAccountId,
        externalMessageId: details.delivery.externalMessageId,
    };
}

/** 构造与已接受投递匹配的归一化回执。 */
function receipt(delivery, occurredAt, overrides = {}) {
    return {
        externalEventId: 'rcpt-1',
        channelAccountId: delivery.channelAccountId,
        externalMessageId: delivery.externalMessageId,
        dedupeKey: 'rcpt-dedupe-1',
        stage: 'delivered',
        occurredAt,
        ...overrides,
    };
}

test('a delivered receipt advances an accepted delivery to delivered', async () => {
    const { gateway, clock } = buildGateway();
    const ctx = await acceptedDelivery(gateway);

    await gateway.application.receipts.record(
        receipt(ctx, clock.addMinutes(clock.now(), 1), { attemptId: 'attempt-9' }),
    );

    const details = await gateway.application.deliveries.find(ctx.deliveryId);
    assert.equal(details.delivery.status, 'delivered');
    assert.equal(details.receipts.length, 1);
    assert.equal(details.receipts[0].stage, 'delivered');
    assert.equal(details.receipts[0].dedupeKey, 'rcpt-dedupe-1');
    assert.equal(details.receipts[0].attemptId, 'attempt-9');
    assert.equal(details.receipts[0].receivedAt, clock.now());
});

test('a duplicate dedupeKey is ignored', async () => {
    const { gateway, clock } = buildGateway();
    const ctx = await acceptedDelivery(gateway);
    const rcpt = receipt(ctx, clock.addMinutes(clock.now(), 1));

    await gateway.application.receipts.record(rcpt);
    await gateway.application.receipts.record(rcpt);

    const details = await gateway.application.deliveries.find(ctx.deliveryId);
    assert.equal(details.receipts.length, 1);
    assert.equal(details.delivery.status, 'delivered');
});

test('a duplicate dedupeKey short-circuits before delivery resolution', async () => {
    const { gateway, clock } = buildGateway();
    const ctx = await acceptedDelivery(gateway);
    const rcpt = receipt(ctx, clock.addMinutes(clock.now(), 1));
    await gateway.application.receipts.record(rcpt);

    await gateway.application.receipts.record({ ...rcpt, externalMessageId: 'unknown-message' });

    const details = await gateway.application.deliveries.find(ctx.deliveryId);
    assert.equal(details.receipts.length, 1);
});

test('a delivered delivery does not regress on a later failed receipt', async () => {
    const { gateway, clock } = buildGateway();
    const ctx = await acceptedDelivery(gateway);
    await gateway.application.receipts.record(receipt(ctx, clock.addMinutes(clock.now(), 1)));
    const failed = receipt(ctx, clock.addMinutes(clock.now(), 2), {
        externalEventId: 'rcpt-2',
        dedupeKey: 'rcpt-dedupe-2',
        stage: 'failed',
    });

    await gateway.application.receipts.record(failed);

    const details = await gateway.application.deliveries.find(ctx.deliveryId);
    assert.equal(details.delivery.status, 'delivered');
    assert.equal(details.receipts.length, 2);
    assert.equal(details.receipts[1].stage, 'failed');
});

test('a failed receipt on an accepted delivery marks it permanent_failed', async () => {
    const { gateway, clock } = buildGateway();
    const ctx = await acceptedDelivery(gateway);

    await gateway.application.receipts.record(receipt(ctx, clock.addMinutes(clock.now(), 1), { stage: 'failed' }));

    const details = await gateway.application.deliveries.find(ctx.deliveryId);
    assert.equal(details.delivery.status, 'permanent_failed');
    assert.equal(details.receipts[0].stage, 'failed');
});

test('a permanent_failed delivery does not regress on further failed receipts', async () => {
    const { gateway, clock } = buildGateway();
    const ctx = await acceptedDelivery(gateway);
    await gateway.application.receipts.record(receipt(ctx, clock.addMinutes(clock.now(), 1), { stage: 'failed' }));
    const second = receipt(ctx, clock.addMinutes(clock.now(), 2), {
        externalEventId: 'rcpt-2',
        dedupeKey: 'rcpt-dedupe-2',
        stage: 'failed',
    });

    await gateway.application.receipts.record(second);

    const details = await gateway.application.deliveries.find(ctx.deliveryId);
    assert.equal(details.delivery.status, 'permanent_failed');
    assert.equal(details.receipts.length, 2);
});

test('a receipt for an unknown message is rejected', async () => {
    const { gateway, clock } = buildGateway();
    const ctx = await acceptedDelivery(gateway);

    const error = await expectRejected(
        () =>
            gateway.application.receipts.record(
                receipt(ctx, clock.addMinutes(clock.now(), 1), { externalMessageId: 'unknown-message' }),
            ),
        'A receipt for an unknown message was not rejected',
    );
    assert.equal(error.code, 'delivery_not_found');
});
