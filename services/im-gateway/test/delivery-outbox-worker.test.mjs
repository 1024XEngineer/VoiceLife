import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DeliveryOutboxWorker } from '../dist/infrastructure/delivery-outbox-worker.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import { ImGatewayError } from '../dist/shared/errors.js';
import { delivery, outboxEvent, T0, T1 } from './persistence-fixtures.mjs';

test('delivery outbox worker dispatches a persisted request and publishes its event', async () => {
    const unitOfWork = new InMemoryImUnitOfWork();
    await unitOfWork.transaction(async (context) => {
        await context.deliveries.save(delivery());
        await context.outbox.append(
            outboxEvent('outbox-requested', {
                eventType: 'im.delivery.requested',
                aggregateId: 'delivery-1',
            }),
        );
    });
    const dispatched = [];
    const logs = [];
    const worker = new DeliveryOutboxWorker({
        unitOfWork,
        clock: new FixedClock(T0),
        dispatch: {
            dispatch: async (deliveryId) => {
                dispatched.push(deliveryId);
                return delivery(deliveryId, { status: 'accepted', externalMessageId: 'platform-1' });
            },
        },
        logger: { log: (entry) => logs.push(entry) },
    });

    assert.equal(await worker.runOnce(), 1);
    assert.deepEqual(dispatched, ['delivery-1']);
    const remaining = await unitOfWork.transaction((context) =>
        context.outbox.claimPending(['im.delivery.requested'], T1, T1, 10),
    );
    assert.deepEqual(remaining, []);
    assert.equal(
        logs.some(
            (entry) =>
                entry.event === 'delivery.worker.dispatched' &&
                entry.deliveryId === 'delivery-1' &&
                entry.correlationId === 'correlation-1',
        ),
        true,
    );
});

test('delivery outbox worker starts with recovery and defers an in-flight delivery', async () => {
    const unitOfWork = new InMemoryImUnitOfWork();
    await unitOfWork.transaction(async (context) => {
        await context.deliveries.save(delivery('delivery-1', { status: 'sending', claimedAt: T0, claimToken: 'live' }));
        await context.outbox.append(
            outboxEvent('outbox-recovery', {
                eventType: 'im.delivery.retry-scheduled',
                aggregateId: 'delivery-1',
            }),
        );
    });
    let dispatchCount = 0;
    const worker = new DeliveryOutboxWorker({
        unitOfWork,
        clock: new FixedClock(T0),
        dispatch: {
            dispatch: async () => {
                dispatchCount += 1;
                return delivery('delivery-1', { status: 'sending', claimedAt: T0, claimToken: 'live' });
            },
        },
        logger: { log: () => {} },
        pollIntervalMs: 10,
    });

    worker.start();
    await new Promise((resolve) => globalThis.setTimeout(resolve, 25));
    await worker.close();
    assert.equal(dispatchCount, 1);
    const beforeLease = await unitOfWork.transaction((context) =>
        context.outbox.claimPending(['im.delivery.retry-scheduled'], T0, T1, 10),
    );
    assert.deepEqual(beforeLease, []);
});

test('delivery outbox worker completes stale events and fails orphaned events without dispatch', async () => {
    const unitOfWork = new InMemoryImUnitOfWork();
    await unitOfWork.transaction(async (context) => {
        await context.deliveries.save(delivery('delivery-complete', { status: 'accepted' }));
        await context.outbox.append(
            outboxEvent('outbox-complete', {
                eventType: 'im.delivery.requested',
                aggregateId: 'delivery-complete',
            }),
        );
        await context.outbox.append(
            outboxEvent('outbox-orphan', {
                eventType: 'im.delivery.requested',
                aggregateId: 'delivery-missing',
            }),
        );
    });
    const logs = [];
    const worker = new DeliveryOutboxWorker({
        unitOfWork,
        clock: new FixedClock(T0),
        dispatch: { dispatch: async () => assert.fail('terminal and orphaned events must not dispatch') },
        logger: { log: (entry) => logs.push(entry) },
    });

    assert.equal(await worker.runOnce(), 2);
    const remaining = await unitOfWork.transaction((context) =>
        context.outbox.claimPending(['im.delivery.requested'], T1, T1, 10),
    );
    assert.deepEqual(remaining, []);
    assert.equal(
        logs.some(
            (entry) =>
                entry.event === 'delivery.worker.event.failed' &&
                entry.deliveryId === 'delivery-missing' &&
                entry.errorCode === 'delivery_not_found',
        ),
        true,
    );
});

test('delivery outbox worker retries a leased event after a concurrent dispatch releases it', async () => {
    const unitOfWork = new InMemoryImUnitOfWork();
    await unitOfWork.transaction(async (context) => {
        await context.deliveries.save(delivery());
        await context.outbox.append(
            outboxEvent('outbox-concurrent', {
                eventType: 'im.delivery.requested',
                aggregateId: 'delivery-1',
            }),
        );
    });
    const clock = new FixedClock(T0);
    let concurrent = true;
    const worker = new DeliveryOutboxWorker({
        unitOfWork,
        clock,
        dispatch: {
            dispatch: async () => {
                if (concurrent) throw new ImGatewayError('invalid_transition', 'already claimed');
                return delivery('delivery-1', { status: 'accepted' });
            },
        },
        logger: {
            log: () => {
                throw new Error('logger unavailable');
            },
        },
    });

    assert.equal(await worker.runOnce(), 1);
    assert.equal(await worker.runOnce(), 0);
    concurrent = false;
    clock.advanceMinutes(3);
    assert.equal(await worker.runOnce(), 1);
    const remaining = await unitOfWork.transaction((context) =>
        context.outbox.claimPending(['im.delivery.requested'], clock.now(), T1, 10),
    );
    assert.deepEqual(remaining, []);
});

test('delivery outbox worker dead-letters a permanent dispatch failure before publishing the event', async () => {
    const unitOfWork = new InMemoryImUnitOfWork();
    await unitOfWork.transaction(async (context) => {
        await context.deliveries.save(delivery('delivery-permanent', { status: 'pending' }));
        await context.outbox.append(
            outboxEvent('outbox-permanent', {
                eventType: 'im.delivery.requested',
                aggregateId: 'delivery-permanent',
            }),
        );
    });
    const logs = [];
    const worker = new DeliveryOutboxWorker({
        unitOfWork,
        clock: new FixedClock(T0),
        dispatch: {
            dispatch: async () =>
                delivery('delivery-permanent', {
                    status: 'permanent_failed',
                    lastErrorCode: 'delivery_target_unavailable',
                }),
            markDeadLetter: async (deliveryId) => {
                const deadLetter = delivery(deliveryId, {
                    status: 'dead_letter',
                    lastErrorCode: 'delivery_target_unavailable',
                });
                await unitOfWork.transaction((context) => context.deliveries.save(deadLetter));
                return deadLetter;
            },
        },
        logger: { log: (entry) => logs.push(entry) },
    });

    assert.equal(await worker.runOnce(), 1);
    const stored = await unitOfWork.transaction((context) => context.deliveries.findById('delivery-permanent'));
    assert.equal(stored.status, 'dead_letter');
    assert.equal(
        logs.some(
            (entry) =>
                entry.event === 'delivery.worker.dead-lettered' && entry.errorCode === 'delivery_target_unavailable',
        ),
        true,
    );
    assert.deepEqual(
        await unitOfWork.transaction((context) => context.outbox.claimPending(['im.delivery.requested'], T1, T1, 10)),
        [],
    );
});

test('delivery outbox worker keeps a business dispatch error recoverable instead of orphaning the delivery', async () => {
    const unitOfWork = new InMemoryImUnitOfWork();
    await unitOfWork.transaction(async (context) => {
        await context.deliveries.save(delivery());
        await context.outbox.append(
            outboxEvent('outbox-target-race', {
                eventType: 'im.delivery.requested',
                aggregateId: 'delivery-1',
            }),
        );
    });
    const clock = new FixedClock(T0);
    const worker = new DeliveryOutboxWorker({
        unitOfWork,
        clock,
        dispatch: {
            dispatch: async () => {
                throw new ImGatewayError('binding_not_found', 'target changed concurrently');
            },
            markDeadLetter: async () => assert.fail('a recoverable event must not be dead-lettered'),
        },
        logger: { log: () => {} },
    });

    assert.equal(await worker.runOnce(), 1);
    clock.advanceMinutes(3);
    const reclaimed = await unitOfWork.transaction((context) =>
        context.outbox.claimPending(['im.delivery.requested'], clock.now(), T1, 10),
    );
    assert.equal(reclaimed.length, 1);
    assert.equal(reclaimed[0].id, 'outbox-target-race');
});

test('delivery outbox worker rejects invalid lifecycle tuning', () => {
    assert.throws(
        () =>
            new DeliveryOutboxWorker({
                unitOfWork: new InMemoryImUnitOfWork(),
                clock: new FixedClock(T0),
                dispatch: { dispatch: async () => delivery() },
                logger: { log: () => {} },
                batchSize: 0,
            }),
        /positive integer/u,
    );
});
