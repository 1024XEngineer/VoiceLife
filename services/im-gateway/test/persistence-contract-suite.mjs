import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    action,
    attempt,
    binding,
    channelAccount,
    delivery,
    externalIdentity,
    inboundEvent,
    intentSubmission,
    outboxEvent,
    pairingSession,
    receipt,
    T0,
    T1,
    T2,
    LATE,
    withUow,
} from './persistence-fixtures.mjs';

/** 与内存实现共享的同一套持久化契约断言。 */
export async function sharedRepositoryContractSuite(makeUow) {
    await test('channel accounts save, find and update by id', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction((ctx) => ctx.channelAccounts.save(channelAccount()));
            const found = await uow.transaction((ctx) => ctx.channelAccounts.findById('channel-1'));
            assert.deepEqual(found, channelAccount());
            await uow.transaction((ctx) =>
                ctx.channelAccounts.save(channelAccount('channel-1', { status: 'disabled' })),
            );
            const updated = await uow.transaction((ctx) => ctx.channelAccounts.findById('channel-1'));
            assert.equal(updated.status, 'disabled');
            assert.equal(updated.updatedAt, T0);
            const missing = await uow.transaction((ctx) => ctx.channelAccounts.findById('channel-unknown'));
            assert.equal(missing, undefined);
        });
    });

    await test('pairing sessions round-trip, pending lookup and expiry query', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.pairingSessions.save(pairingSession());
                await ctx.pairingSessions.save(
                    pairingSession('pairing-confirmed', { status: 'confirmed', displayCodeHash: 'hash-confirmed' }),
                );
                await ctx.pairingSessions.save(pairingSession('pairing-future', { expiresAt: T2 }));
                await ctx.pairingSessions.save(pairingSession('pairing-expired', { expiresAt: T0 }));
            });
            const found = await uow.transaction((ctx) => ctx.pairingSessions.findById('pairing-1'));
            assert.deepEqual(found, pairingSession());
            const pending = await uow.transaction((ctx) =>
                ctx.pairingSessions.findPendingByDisplayCodeHash('hash-1234'),
            );
            assert.equal(pending.id, 'pairing-1');
            const notPending = await uow.transaction((ctx) =>
                ctx.pairingSessions.findPendingByDisplayCodeHash('hash-confirmed'),
            );
            assert.equal(notPending, undefined);
            const expired = await uow.transaction((ctx) => ctx.pairingSessions.findExpiredPairingSessions(T2));
            assert.deepEqual([...expired.map((session) => session.id)].sort(), ['pairing-expired', 'pairing-future']);
        });
    });

    await test('external identities round-trip and channel-and-hash lookup', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.identities.save(externalIdentity());
                await ctx.identities.save(externalIdentity('identity-2', { externalUserIdHash: 'hash-open-id-2' }));
            });
            const found = await uow.transaction((ctx) => ctx.identities.findById('identity-1'));
            assert.deepEqual(found, externalIdentity());
            const byHash = await uow.transaction((ctx) =>
                ctx.identities.findByChannelAndHash('channel-1', 'hash-open-id-2'),
            );
            assert.equal(byHash.id, 'identity-2');
            const wrongChannel = await uow.transaction((ctx) =>
                ctx.identities.findByChannelAndHash('channel-other', 'hash-open-id'),
            );
            assert.equal(wrongChannel, undefined);
        });
    });

    await test('bindings round-trip, priority ordering and device/identity lookups', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.bindings.save(binding());
                await ctx.bindings.save(binding('binding-2', { priority: 5, externalIdentityId: 'identity-2' }));
                await ctx.bindings.save(binding('binding-unbound', { status: 'unbound' }));
            });
            const found = await uow.transaction((ctx) => ctx.bindings.findById('binding-1'));
            assert.deepEqual(found, binding());
            const active = await uow.transaction((ctx) => ctx.bindings.listActiveByUser('user-1'));
            assert.deepEqual(
                active.map((item) => item.id),
                ['binding-2', 'binding-1'],
            );
            const byDevice = await uow.transaction((ctx) => ctx.bindings.findActiveByDevice('device-1'));
            assert.equal(byDevice.length, 2);
            const byIdentity = await uow.transaction((ctx) => ctx.bindings.findActiveByIdentity('identity-1'));
            assert.equal(byIdentity.id, 'binding-1');
            await uow.transaction((ctx) => ctx.bindings.save(binding('binding-1', { status: 'unbound' })));
            const afterUnbind = await uow.transaction((ctx) => ctx.bindings.findActiveByIdentity('identity-1'));
            assert.equal(afterUnbind, undefined);
        });
    });

    await test('inbound events round-trip by id and composite external key', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction((ctx) => ctx.inboundEvents.save(inboundEvent()));
            const found = await uow.transaction((ctx) => ctx.inboundEvents.findById('inbound-1'));
            assert.deepEqual(found, inboundEvent());
            const byExternal = await uow.transaction((ctx) =>
                ctx.inboundEvents.findByExternalEvent('channel-1', 'external-1'),
            );
            assert.equal(byExternal.id, 'inbound-1');
            const wrongChannel = await uow.transaction((ctx) =>
                ctx.inboundEvents.findByExternalEvent('channel-other', 'external-1'),
            );
            assert.equal(wrongChannel, undefined);
            await uow.transaction((ctx) => ctx.inboundEvents.save(inboundEvent('inbound-1', { status: 'processed' })));
            const updated = await uow.transaction((ctx) =>
                ctx.inboundEvents.findByExternalEvent('channel-1', 'external-1'),
            );
            assert.equal(updated.status, 'processed');
        });
    });

    await test('intent submissions round-trip by business key and update', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction((ctx) => ctx.intentSubmissions.save(intentSubmission()));
            const found = await uow.transaction((ctx) =>
                ctx.intentSubmissions.findByBusinessKey('event-1', 'reminder_due'),
            );
            assert.deepEqual(found, intentSubmission());
            const otherKind = await uow.transaction((ctx) =>
                ctx.intentSubmissions.findByBusinessKey('event-1', 'schedule_receipt'),
            );
            assert.equal(otherKind, undefined);
            await uow.transaction((ctx) =>
                ctx.intentSubmissions.save(intentSubmission('event-1', { requestFingerprint: 'fingerprint-2' })),
            );
            const updated = await uow.transaction((ctx) =>
                ctx.intentSubmissions.findByBusinessKey('event-1', 'reminder_due'),
            );
            assert.equal(updated.requestFingerprint, 'fingerprint-2');
        });
    });

    await test('deliveries round-trip with business key, external message and action window queries', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.deliveries.save(delivery());
                await ctx.deliveries.save(
                    delivery('delivery-weak', {
                        businessEventId: 'event-weak',
                        kind: 'reminder_due',
                        semanticPayload: {
                            businessEventId: 'event-weak',
                            reminderType: 'weak',
                            reminderTriggerId: 'trigger-1',
                            recipient: { userId: 'user-1', deviceId: 'device-1' },
                        },
                        externalMessageId: 'platform-weak',
                    }),
                );
                await ctx.deliveries.save(
                    delivery('delivery-other-device', {
                        businessEventId: 'event-other',
                        semanticPayload: {
                            businessEventId: 'event-other',
                            reminderType: 'strong',
                            reminderTriggerId: 'trigger-1',
                            recipient: { userId: 'user-1', deviceId: 'device-other' },
                        },
                    }),
                );
                await ctx.deliveries.save(
                    delivery('delivery-expired', {
                        businessEventId: 'event-expired',
                        expiresAt: T0,
                    }),
                );
            });
            const found = await uow.transaction((ctx) => ctx.deliveries.findById('delivery-1'));
            assert.deepEqual(found, delivery());
            const byKey = await uow.transaction((ctx) =>
                ctx.deliveries.findByBusinessKey('event-1', 'binding-1', 'reminder_due'),
            );
            assert.equal(byKey.id, 'delivery-1');
            const byMessage = await uow.transaction((ctx) =>
                ctx.deliveries.findByExternalMessage('channel-1', 'platform-weak'),
            );
            assert.equal(byMessage.id, 'delivery-weak');
            const activeWindow = await uow.transaction((ctx) =>
                ctx.deliveries.findActiveActionWindow('device-1', 'trigger-1', T1),
            );
            assert.equal(activeWindow.id, 'delivery-1');
            const weakWindow = await uow.transaction((ctx) =>
                ctx.deliveries.findActiveActionWindow('device-1', 'trigger-1', LATE),
            );
            assert.equal(weakWindow, undefined);
            await uow.transaction((ctx) => ctx.deliveries.save(delivery('delivery-1', { status: 'delivered' })));
            const updated = await uow.transaction((ctx) => ctx.deliveries.findById('delivery-1'));
            assert.equal(updated.status, 'delivered');
        });
    });

    await test('delivery attempts round-trip, numbering and per-delivery listing', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.deliveries.save(delivery());
                await ctx.deliveries.saveAttempt(attempt('attempt-1', 'delivery-1'));
                await ctx.deliveries.saveAttempt(
                    attempt('attempt-2', 'delivery-1', { attemptNo: 2, status: 'retryable_failed' }),
                );
            });
            const first = await uow.transaction((ctx) => ctx.deliveries.findAttempt('delivery-1', 1));
            assert.deepEqual(first, attempt());
            const nextNo = await uow.transaction((ctx) => ctx.deliveries.nextAttemptNo('delivery-1'));
            assert.equal(nextNo, 3);
            const emptyNo = await uow.transaction((ctx) => ctx.deliveries.nextAttemptNo('delivery-unknown'));
            assert.equal(emptyNo, 1);
            const attempts = await uow.transaction((ctx) => ctx.deliveries.listAttempts('delivery-1'));
            assert.deepEqual(
                attempts.map((item) => item.attemptNo),
                [1, 2],
            );
            const byAttemptMessage = await uow.transaction((ctx) =>
                ctx.deliveries.findByExternalMessage('channel-1', 'platform-msg-1'),
            );
            assert.equal(byAttemptMessage.id, 'delivery-1');
        });
    });

    await test('delivery receipts round-trip, dedupe lookup and per-delivery listing', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.deliveries.save(delivery());
                await ctx.deliveries.saveReceipt(receipt());
            });
            const found = await uow.transaction((ctx) => ctx.deliveries.findReceiptByDedupeKey('dedupe-1'));
            assert.deepEqual(found, receipt());
            const missing = await uow.transaction((ctx) => ctx.deliveries.findReceiptByDedupeKey('dedupe-unknown'));
            assert.equal(missing, undefined);
            const listed = await uow.transaction((ctx) => ctx.deliveries.listReceipts('delivery-1'));
            assert.equal(listed.length, 1);
        });
    });

    await test('actions round-trip and operation, key-hash and pending lookups', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.actions.save(action());
                await ctx.actions.save(
                    action('action-2', {
                        operationId: 'operation-2',
                        actionKeyHash: 'hash-action-2',
                        status: 'dispatched',
                        dispatchedAt: T1,
                        result: {
                            schemaVersion: '1',
                            operationId: 'operation-2',
                            reminderTriggerId: 'trigger-1',
                            status: 'succeeded',
                            occurredAt: T1,
                        },
                    }),
                );
                await ctx.actions.save(
                    action('action-other-device', {
                        operationId: 'operation-3',
                        actionKeyHash: 'hash-action-3',
                        deviceId: 'device-other',
                        reminderTriggerId: 'trigger-1',
                    }),
                );
                await ctx.actions.save(
                    action('action-other-trigger', {
                        operationId: 'operation-4',
                        actionKeyHash: 'hash-action-4',
                        reminderTriggerId: 'trigger-other',
                    }),
                );
                await ctx.actions.save(
                    action('action-expired', {
                        operationId: 'operation-5',
                        actionKeyHash: 'hash-action-5',
                        expiresAt: T0,
                    }),
                );
            });
            const found = await uow.transaction((ctx) => ctx.actions.findById('action-1'));
            assert.deepEqual(found, action());
            const withResult = await uow.transaction((ctx) => ctx.actions.findById('action-2'));
            assert.deepEqual(withResult.result, {
                schemaVersion: '1',
                operationId: 'operation-2',
                reminderTriggerId: 'trigger-1',
                status: 'succeeded',
                occurredAt: T1,
            });
            const byOperation = await uow.transaction((ctx) => ctx.actions.findByOperationId('operation-2'));
            assert.equal(byOperation.id, 'action-2');
            const byKeyHash = await uow.transaction((ctx) => ctx.actions.findByActionKeyHash('hash-action-2'));
            assert.equal(byKeyHash.id, 'action-2');
            const pending = await uow.transaction((ctx) =>
                ctx.actions.findPendingByDeviceAndTrigger('device-1', 'trigger-1', T1),
            );
            assert.deepEqual(
                pending.map((item) => item.id),
                ['action-1', 'action-2'],
            );
            const expired = await uow.transaction((ctx) => ctx.actions.findExpiredActions(T1));
            assert.deepEqual(
                expired.map((item) => item.id),
                ['action-expired'],
            );
        });
    });

    await test('outbox appends without error', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction((ctx) => ctx.outbox.append(outboxEvent()));
        });
    });
}
