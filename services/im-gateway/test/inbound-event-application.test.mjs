import { test } from 'node:test';
import assert from 'node:assert/strict';

import { createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import { expectGatewayError } from './helpers.mjs';

class ExposedUnitOfWork extends InMemoryImUnitOfWork {
    inbound(eventId) {
        return [...this.inboundRows.values()].find((event) => event.id === eventId);
    }
}

function inboundGateway() {
    const clock = new FixedClock();
    const uow = new ExposedUnitOfWork();
    const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: uow });
    return { gateway, clock, uow };
}

function event(overrides = {}) {
    return {
        id: 'inbound-1',
        externalEventId: 'external-1',
        platform: 'wechat_official',
        channelAccountId: 'channel-1',
        type: 'message.received',
        payload: { text: 'hello' },
        occurredAt: '2026-08-03T00:00:00.000Z',
        ...overrides,
    };
}

test('recordIfNew persists a received event keyed by channel and external event', async () => {
    const { gateway, clock, uow } = inboundGateway();

    const result = await gateway.application.inboundEvents.recordIfNew(event());

    assert.equal(result, 'accepted');
    const stored = uow.inbound('inbound-1');
    assert.equal(stored.channelAccountId, 'channel-1');
    assert.equal(stored.externalEventId, 'external-1');
    assert.equal(stored.eventType, 'message.received');
    assert.deepEqual(stored.payload, { text: 'hello' });
    assert.equal(stored.status, 'received');
    assert.equal(stored.receivedAt, clock.now());
});

test('recordIfNew deduplicates by channel plus external event only', async () => {
    const { gateway } = inboundGateway();

    assert.equal(await gateway.application.inboundEvents.recordIfNew(event()), 'accepted');
    assert.equal(await gateway.application.inboundEvents.recordIfNew(event({ id: 'inbound-duplicate' })), 'duplicate');
    assert.equal(
        await gateway.application.inboundEvents.recordIfNew(
            event({ id: 'inbound-other-channel', channelAccountId: 'channel-2' }),
        ),
        'accepted',
    );
});

test('a failed inbound event can be accepted again with the latest payload', async () => {
    const { gateway, clock, uow } = inboundGateway();
    await gateway.application.inboundEvents.recordIfNew(event());
    await gateway.application.inboundEvents.markFailed('inbound-1');
    clock.advanceMinutes(1);

    const result = await gateway.application.inboundEvents.recordIfNew(
        event({ id: 'inbound-retry', payload: { text: 'retry' } }),
    );

    assert.equal(result, 'accepted');
    assert.equal(uow.inbound('inbound-1'), undefined);
    const retried = uow.inbound('inbound-retry');
    assert.equal(retried.status, 'received');
    assert.deepEqual(retried.payload, { text: 'retry' });
    assert.equal(retried.receivedAt, clock.now());
});

test('status transitions update the persisted inbound event', async () => {
    const { gateway, uow } = inboundGateway();
    await gateway.application.inboundEvents.recordIfNew(event());

    await gateway.application.inboundEvents.markProcessing('inbound-1');
    assert.equal(uow.inbound('inbound-1').status, 'processing');
    await gateway.application.inboundEvents.markProcessed('inbound-1');
    assert.equal(uow.inbound('inbound-1').status, 'processed');
    await gateway.application.inboundEvents.markFailed('inbound-1');
    assert.equal(uow.inbound('inbound-1').status, 'failed');
});

test('status transitions reject an unknown inbound event', async () => {
    const { gateway } = inboundGateway();

    for (const transition of ['markProcessing', 'markProcessed', 'markFailed']) {
        await expectGatewayError(
            () => gateway.application.inboundEvents[transition]('inbound-missing'),
            'invalid_transition',
            `${transition} accepted an unknown inbound event`,
        );
    }
});
