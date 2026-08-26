import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    action,
    attempt,
    binding,
    channelAccount,
    delivery,
    device,
    externalIdentity,
    inboundEvent,
    intentSubmission,
    outboxEvent,
    pairingSession,
    receipt,
    reminderActionFact,
    T0,
    T1,
    T2,
    LATE,
    withUow,
} from './persistence-fixtures.mjs';

/** 与内存实现共享的同一套持久化契约断言。 */
export async function sharedRepositoryContractSuite(makeUow) {
    await test('devices create, digest lookup, immutable ownership and list safely', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction((ctx) => ctx.devices.create(device()));
            const byDigest = await uow.transaction((ctx) => ctx.devices.findByTokenDigest(new Uint8Array(32).fill(7)));
            assert.equal(byDigest.deviceId, 'device-1');
            await uow.transaction((ctx) =>
                ctx.devices.save(
                    device('device-1', {
                        userId: 'user-other',
                        status: 'revoked',
                        tokenDigest: new Uint8Array(32).fill(8),
                    }),
                ),
            );
            const found = await uow.transaction((ctx) => ctx.devices.findById('device-1'));
            assert.equal(found.userId, 'user-1');
            assert.equal(found.status, 'revoked');
            assert.deepEqual([...found.tokenDigest], [...new Uint8Array(32).fill(8)]);
            assert.deepEqual(
                (await uow.transaction((ctx) => ctx.devices.list('user-1'))).map((item) => item.deviceId),
                ['device-1'],
            );
            assert.deepEqual(await uow.transaction((ctx) => ctx.devices.list('user-other')), []);
        });
    });

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
                    pairingSession('pairing-confirmed', {
                        status: 'confirmed',
                        displayCodeHash: 'hash-confirmed',
                        confirmedAt: T1,
                    }),
                );
                await ctx.pairingSessions.save(
                    pairingSession('pairing-future', {
                        displayCodeHash: 'hash-future',
                        deviceId: 'device-future',
                        expiresAt: T2,
                    }),
                );
                await ctx.pairingSessions.save(
                    pairingSession('pairing-expired', {
                        displayCodeHash: 'hash-expired',
                        deviceId: 'device-expired',
                        createdAt: '2026-08-02T23:50:00.000Z',
                        expiresAt: T0,
                    }),
                );
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

    await test('pairing sessions atomically reserve a pending display-code hash', async () => {
        await withUow(makeUow, async (uow) => {
            const [first, second] = await Promise.all([
                uow.transaction((ctx) => ctx.pairingSessions.createPendingIfAbsent(pairingSession('pairing-a'))),
                uow.transaction((ctx) => ctx.pairingSessions.createPendingIfAbsent(pairingSession('pairing-b'))),
            ]);
            assert.equal([first, second].filter(Boolean).length, 1);
            const found = await uow.transaction((ctx) => ctx.pairingSessions.findPendingByDisplayCodeHash('hash-1234'));
            assert.equal(found.id, first ? 'pairing-a' : 'pairing-b');
        });
    });

    await test('pairing sessions cancel the previous pending session for a device', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                assert.equal(await ctx.pairingSessions.createPendingIfAbsent(pairingSession('pairing-old')), true);
                await ctx.pairingSessions.cancelPendingByDevice('device-1');
                assert.equal(
                    await ctx.pairingSessions.createPendingIfAbsent(
                        pairingSession('pairing-new', { displayCodeHash: 'hash-new' }),
                    ),
                    true,
                );
            });
            const oldSession = await uow.transaction((ctx) => ctx.pairingSessions.findById('pairing-old'));
            const newSession = await uow.transaction((ctx) => ctx.pairingSessions.findById('pairing-new'));
            assert.equal(oldSession.status, 'cancelled');
            assert.equal(newSession.status, 'pending');
        });
    });

    await test('terminal pairing transitions never overwrite a concurrently confirmed session', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.pairingSessions.save(pairingSession());
                assert.equal(await ctx.pairingSessions.transitionPending('pairing-1', 'cancelled'), true);
                assert.equal(await ctx.pairingSessions.transitionPending('pairing-1', 'expired'), false);
                await ctx.pairingSessions.save(
                    pairingSession('pairing-confirmed-race', {
                        status: 'confirmed',
                        displayCodeHash: 'hash-confirmed-race',
                        confirmedAt: T1,
                    }),
                );
                assert.equal(await ctx.pairingSessions.transitionPending('pairing-confirmed-race', 'cancelled'), false);
            });
            const confirmed = await uow.transaction((ctx) => ctx.pairingSessions.findById('pairing-confirmed-race'));
            assert.equal(confirmed.status, 'confirmed');
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
                await ctx.bindings.save(
                    binding('binding-2', { priority: 5, deviceId: 'device-2', externalIdentityId: 'identity-2' }),
                );
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
            assert.equal(byDevice.length, 1);
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
            await uow.transaction((ctx) =>
                ctx.inboundEvents.save(
                    inboundEvent('inbound-2', {
                        channelAccountId: 'channel-2',
                        externalEventId: 'external-1',
                    }),
                ),
            );
            const otherChannel = await uow.transaction((ctx) =>
                ctx.inboundEvents.findByExternalEvent('channel-2', 'external-1'),
            );
            assert.equal(otherChannel.id, 'inbound-2');
            await uow.transaction((ctx) => ctx.inboundEvents.save(inboundEvent('inbound-1', { status: 'processed' })));
            const updated = await uow.transaction((ctx) =>
                ctx.inboundEvents.findByExternalEvent('channel-1', 'external-1'),
            );
            assert.equal(updated.status, 'processed');
        });
    });

    await test('intent submissions claim once by business key and finalize the winner', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                const first = await ctx.intentSubmissions.createIfAbsent(intentSubmission());
                assert.deepEqual(first, { created: true, record: intentSubmission() });
                // 同键再次预占：返回首条记录而非覆盖
                const again = await ctx.intentSubmissions.createIfAbsent(intentSubmission());
                assert.deepEqual(again, { created: false, record: intentSubmission() });
                const otherKind = await ctx.intentSubmissions.createIfAbsent(
                    intentSubmission('event-1', { kind: 'schedule_receipt' }),
                );
                assert.equal(otherKind.created, true);
                // 仅 claim 持有者回填最终受理结果
                await ctx.intentSubmissions.finalizeClaim(
                    intentSubmission('event-1', { requestFingerprint: 'fingerprint-2' }),
                );
            });
            const found = await uow.transaction((ctx) =>
                ctx.intentSubmissions.findByBusinessKey('event-1', 'reminder_due'),
            );
            assert.equal(found.requestFingerprint, 'fingerprint-2');
            const otherKind = await uow.transaction((ctx) =>
                ctx.intentSubmissions.findByBusinessKey('event-1', 'schedule_receipt'),
            );
            assert.equal(otherKind.requestFingerprint, 'fingerprint-1');
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

    await test('delivery receipts round-trip, dedupe lookup, first-write-wins and per-delivery listing', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.deliveries.save(delivery());
                await ctx.deliveries.saveReceipt(receipt());
                // 重复 webhook：同一 dedupe_key、不同 id，首写保留
                await ctx.deliveries.saveReceipt(
                    receipt('receipt-other', { externalEventId: 'platform-receipt-other' }),
                );
            });
            const found = await uow.transaction((ctx) => ctx.deliveries.findReceiptByDedupeKey('dedupe-1'));
            assert.equal(found.id, 'receipt-1');
            assert.equal(found.externalEventId, 'platform-receipt-1');
            const missing = await uow.transaction((ctx) => ctx.deliveries.findReceiptByDedupeKey('dedupe-unknown'));
            assert.equal(missing, undefined);
            const listed = await uow.transaction((ctx) => ctx.deliveries.listReceipts('delivery-1'));
            assert.equal(listed.length, 1);
        });
    });

    await test('createIfAbsent keeps the first delivery for a business key and reports creation', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                const first = await ctx.deliveries.createIfAbsent(delivery());
                assert.deepEqual(first, { id: 'delivery-1', created: true });
                const second = await ctx.deliveries.createIfAbsent(
                    delivery('delivery-other', { businessEventId: 'event-1' }),
                );
                assert.deepEqual(second, { id: 'delivery-1', created: false });
            });
            const existing = await uow.transaction((ctx) => ctx.deliveries.findById('delivery-1'));
            assert.equal(existing.id, 'delivery-1');
            const notInserted = await uow.transaction((ctx) => ctx.deliveries.findById('delivery-other'));
            assert.equal(notInserted, undefined);
            const byKey = await uow.transaction((ctx) =>
                ctx.deliveries.findByBusinessKey('event-1', 'binding-1', 'reminder_due'),
            );
            assert.equal(byKey.id, 'delivery-1');
        });
    });

    await test('claimForDispatch atomically claims a pending delivery once', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.deliveries.save(delivery());
                const claimed = await ctx.deliveries.claimForDispatch('delivery-1', T1, 60);
                assert.equal(claimed.id, 'delivery-1');
                assert.equal(claimed.status, 'sending');
                assert.equal(claimed.claimedAt, T1);
                assert.notEqual(claimed.claimToken, undefined);
                const again = await ctx.deliveries.claimForDispatch('delivery-1', T1, 60);
                assert.equal(again, undefined);
            });
            const after = await uow.transaction((ctx) => ctx.deliveries.findById('delivery-1'));
            assert.equal(after.status, 'sending');
        });
    });

    await test('claimForDispatch reclaims an expired sending claim and refuses a live one', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.deliveries.save(delivery());
                const first = await ctx.deliveries.claimForDispatch('delivery-1', T0, 60);
                // 同一时刻重领：lease 未过期，拒绝
                const live = await ctx.deliveries.claimForDispatch('delivery-1', T0, 60);
                assert.equal(live, undefined);
                // lease 到期后重领：换发新 claim token
                const reclaimed = await ctx.deliveries.claimForDispatch('delivery-1', T2, 60);
                assert.notEqual(reclaimed, undefined);
                assert.notEqual(reclaimed.claimToken, first.claimToken);
                assert.equal(reclaimed.claimedAt, T2);
            });
        });
    });

    await test('claimForDispatch rejects deliveries not in a dispatchable state', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.deliveries.save(delivery('delivery-accepted', { status: 'accepted' }));
                await ctx.deliveries.save(
                    delivery('delivery-expired', { businessEventId: 'event-expired', status: 'dead_letter' }),
                );
                assert.equal(await ctx.deliveries.claimForDispatch('delivery-accepted', T1, 60), undefined);
                assert.equal(await ctx.deliveries.claimForDispatch('delivery-expired', T1, 60), undefined);
                assert.equal(await ctx.deliveries.claimForDispatch('delivery-missing', T1, 60), undefined);
            });
        });
    });

    await test('saveIfClaimed fences stale workers and clears ownership on terminal state', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.deliveries.save(delivery());
                const claimA = await ctx.deliveries.claimForDispatch('delivery-1', T0, 60);
                // 另一个 worker 在 lease 过期后重领
                const claimB = await ctx.deliveries.claimForDispatch('delivery-1', T2, 60);
                // 旧 worker A 的迟到写被 fenced
                const stale = await ctx.deliveries.saveIfClaimed(
                    delivery('delivery-1', {
                        status: 'retryable_failed',
                        lastErrorCode: 'pre_send_exception',
                        updatedAt: T2,
                    }),
                    claimA.claimToken,
                );
                assert.equal(stale, undefined);
                // 当前 owner B 的终态写生效
                const applied = await ctx.deliveries.saveIfClaimed(
                    delivery('delivery-1', { status: 'accepted', externalMessageId: 'platform-1', updatedAt: T2 }),
                    claimB.claimToken,
                );
                assert.equal(applied.status, 'accepted');
                assert.equal(applied.externalMessageId, 'platform-1');
            });
            const after = await uow.transaction((ctx) => ctx.deliveries.findById('delivery-1'));
            assert.equal(after.status, 'accepted');
            assert.equal(after.claimedAt, undefined);
            assert.equal(after.claimToken, undefined);
        });
    });

    await test('saveIfClaimed refuses writes once the delivery leaves sending', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.deliveries.save(delivery());
                const claim = await ctx.deliveries.claimForDispatch('delivery-1', T0, 60);
                const applied = await ctx.deliveries.saveIfClaimed(
                    delivery('delivery-1', { status: 'accepted', updatedAt: T1 }),
                    claim.claimToken,
                );
                assert.equal(applied.status, 'accepted');
                // 已离开 sending，同 token 再写拒绝
                const again = await ctx.deliveries.saveIfClaimed(
                    delivery('delivery-1', { status: 'retryable_failed', updatedAt: T1 }),
                    claim.claimToken,
                );
                assert.equal(again, undefined);
            });
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

    await test('createIfAbsent keeps the first action for an idempotency key', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                const first = await ctx.actions.createIfAbsent(action());
                assert.deepEqual(first, { action: action(), created: true });
                const second = await ctx.actions.createIfAbsent(
                    action('action-other', {
                        operationId: 'operation-other',
                        actionKeyHash: 'hash-action-1',
                        actionParams: { minutes: 60 },
                    }),
                );
                assert.deepEqual(second, { action: action(), created: false });
            });
            const stored = await uow.transaction((ctx) => ctx.actions.findById('action-1'));
            assert.deepEqual(stored, action());
            const notInserted = await uow.transaction((ctx) => ctx.actions.findById('action-other'));
            assert.equal(notInserted, undefined);
        });
    });

    await test('device voice action facts persist by event and operation while latest is ordered by occurredAt', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                const first = reminderActionFact();
                assert.deepEqual(await ctx.reminderActionFacts.createIfAbsent(first), { fact: first, created: true });
                assert.deepEqual(await ctx.reminderActionFacts.createIfAbsent(first), { fact: first, created: false });
                await ctx.reminderActionFacts.createIfAbsent(
                    reminderActionFact('voice-event-old', {
                        fingerprint: 'fingerprint:voice-event-old',
                        report: {
                            ...first.report,
                            eventId: 'voice-event-old',
                            operationId: 'voice-operation-old',
                            occurredAt: T0,
                        },
                        receivedAt: T2,
                    }),
                );
            });
            const byEvent = await uow.transaction((ctx) => ctx.reminderActionFacts.findByEventId('voice-event-1'));
            assert.deepEqual(byEvent, reminderActionFact());
            const byOperation = await uow.transaction((ctx) =>
                ctx.reminderActionFacts.findByDeviceAndOperationId('device-1', 'voice-operation-1'),
            );
            assert.equal(byOperation.eventId, 'voice-event-1');
            const byTime = await uow.transaction((ctx) =>
                ctx.reminderActionFacts.findByDeviceTriggerAndOccurredAt('device-1', 'trigger-1', T1),
            );
            assert.equal(byTime.eventId, 'voice-event-1');
            const latest = await uow.transaction((ctx) =>
                ctx.reminderActionFacts.findLatestByDeviceAndTrigger('device-1', 'trigger-1'),
            );
            assert.equal(latest.eventId, 'voice-event-1');
        });
    });

    await test('outbox claims due events with a lease and publishes them', async () => {
        await withUow(makeUow, async (uow) => {
            await uow.transaction(async (ctx) => {
                await ctx.outbox.append(outboxEvent('outbox-due', { eventType: 'im.delivery.requested' }));
                await ctx.outbox.append(
                    outboxEvent('outbox-future', {
                        eventType: 'im.delivery.retry-scheduled',
                        availableAt: T2,
                    }),
                );
                await ctx.outbox.append(outboxEvent('outbox-unrelated', { eventType: 'audit.created' }));
            });

            const claimed = await uow.transaction((ctx) =>
                ctx.outbox.claimPending(['im.delivery.requested', 'im.delivery.retry-scheduled'], T1, T2, 10),
            );
            assert.deepEqual(
                claimed.map((event) => ({ id: event.id, attempts: event.attempts, availableAt: event.availableAt })),
                [{ id: 'outbox-due', attempts: 1, availableAt: T2 }],
            );

            const leased = await uow.transaction((ctx) =>
                ctx.outbox.claimPending(['im.delivery.requested'], T1, T2, 10),
            );
            assert.deepEqual(leased, []);

            await uow.transaction((ctx) => ctx.outbox.markPublished('outbox-due', T1));
            const afterPublish = await uow.transaction((ctx) =>
                ctx.outbox.claimPending(['im.delivery.requested'], T2, LATE, 10),
            );
            assert.deepEqual(afterPublish, []);
        });
    });
}
