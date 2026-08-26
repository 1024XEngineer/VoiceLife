import assert from 'node:assert/strict';
import { Buffer } from 'node:buffer';
import { describe, test } from 'node:test';
import { clearTimeout, setTimeout } from 'node:timers';

import {
    createMockImGateway,
    createPostgresImGateway,
    DeviceManagementService,
    mockImGatewayPorts,
} from '../dist/index.js';
import { DeliveryOutboxWorker } from '../dist/infrastructure/delivery-outbox-worker.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';
import { IM_TABLES, SCHEMA_VERSION } from '../dist/infrastructure/persistence/postgres/schema.js';
import { bindFixtureUser, strongIntent, weakIntent } from './helpers.mjs';
import {
    action,
    attempt,
    channelAccount,
    delivery,
    makeInMemoryUow,
    makePostgresUow,
    outboxEvent,
    receipt,
    reminderActionFact,
    POSTGRES_URL,
    T0,
    T1,
    T2,
    withUow,
} from './persistence-fixtures.mjs';
import { sharedRepositoryContractSuite } from './persistence-contract-suite.mjs';

/** 检测 Postgres 是否可用，不可用时返回 null 并跳过整套 Postgres 契约。 */
async function withTimeout(promise, milliseconds, label) {
    let timer;
    try {
        return await Promise.race([
            promise,
            new Promise((_, reject) => {
                timer = setTimeout(() => reject(new Error(`${label} timed out`)), milliseconds);
            }),
        ]);
    } finally {
        clearTimeout(timer);
    }
}

async function probePostgres(url) {
    const probe = new PostgresImUnitOfWork(url);
    try {
        await probe.runRaw('SELECT 1');
        return probe;
    } catch {
        await probe.close().catch(() => {});
        return null;
    }
}

describe('IM Gateway persistence contract', async () => {
    describe('in-memory', async () => {
        await sharedRepositoryContractSuite(makeInMemoryUow);
    });
});

const postgresProbe = await probePostgres(POSTGRES_URL);

describe(
    'PostgreSQL',
    { skip: postgresProbe === null && 'PostgreSQL 未运行，跳过 Postgres 契约测试（docker compose up -d 启动）' },
    async () => {
        await postgresProbe.close();
        await sharedRepositoryContractSuite(() => makePostgresUow());

        await test('a committed transaction persists changes', async () => {
            await withUow(
                () => makePostgresUow(),
                async (uow) => {
                    await uow.transaction((ctx) => ctx.channelAccounts.save(channelAccount()));
                    const found = await uow.transaction((ctx) => ctx.channelAccounts.findById('channel-1'));
                    assert.deepEqual(found, channelAccount());
                },
            );
        });

        await test('a failed transaction rolls back Delivery and Outbox together without half-written state', async () => {
            await withUow(
                () => makePostgresUow(),
                async (uow) => {
                    await assert.rejects(
                        uow.transaction(async (ctx) => {
                            await ctx.deliveries.save(delivery());
                            await ctx.outbox.append(outboxEvent());
                            throw new Error('boom');
                        }),
                        'The failing transaction did not reject',
                    );
                    const rows = await uow.runRaw(
                        `SELECT (SELECT COUNT(*)::int FROM im_deliveries) AS deliveries,
                            (SELECT COUNT(*)::int FROM im_outbox_events) AS outbox`,
                    );
                    assert.deepEqual(rows[0], { deliveries: 0, outbox: 0 });
                    const found = await uow.transaction((ctx) => ctx.deliveries.findById('delivery-1'));
                    assert.equal(found, undefined);
                },
            );
        });

        await test('an unconfirmed Action is recoverable from a fresh process connection', async () => {
            const first = new PostgresImUnitOfWork(POSTGRES_URL);
            await first.migrate();
            await first.truncateAll();
            await first.transaction((ctx) => ctx.actions.save(action()));
            await first.close();

            const second = new PostgresImUnitOfWork(POSTGRES_URL);
            const recovered = await second.transaction((ctx) =>
                ctx.actions.findPendingByDeviceAndTrigger('device-1', 'trigger-1', T1),
            );
            assert.deepEqual(
                recovered.map((item) => item.id),
                ['action-1'],
            );
            await second.close();
        });

        await test('a persisted voice action fact is recoverable and operation-id unique after process restart', async () => {
            const first = await makePostgresUow();
            await first.transaction((ctx) => ctx.reminderActionFacts.save(reminderActionFact()));
            await first.close();

            const second = new PostgresImUnitOfWork(POSTGRES_URL);
            const recovered = await second.transaction((ctx) =>
                ctx.reminderActionFacts.findByDeviceAndOperationId('device-1', 'voice-operation-1'),
            );
            assert.equal(recovered.eventId, 'voice-event-1');
            const duplicate = await second.transaction((ctx) =>
                ctx.reminderActionFacts.createIfAbsent(
                    reminderActionFact('voice-event-conflict', {
                        fingerprint: 'fingerprint:voice-event-conflict',
                        report: {
                            ...reminderActionFact().report,
                            eventId: 'voice-event-conflict',
                        },
                    }),
                ),
            );
            assert.equal(duplicate.created, false);
            assert.equal(duplicate.fact.eventId, 'voice-event-1');
            await second.close();
        });

        await test('timestamps persist as UTC timestamptz and round-trip without zone shift', async () => {
            await withUow(
                () => makePostgresUow(),
                async (uow) => {
                    await uow.transaction((ctx) => ctx.deliveries.save(delivery()));
                    const found = await uow.transaction((ctx) => ctx.deliveries.findById('delivery-1'));
                    assert.equal(found.createdAt, T0);
                    assert.equal(found.updatedAt, T0);
                    assert.equal(found.expiresAt, T2);
                    const column = await uow.runRaw(
                        `SELECT data_type FROM information_schema.columns
                     WHERE table_name = 'im_deliveries' AND column_name = 'created_at'`,
                    );
                    assert.equal(column[0].data_type, 'timestamp with time zone');
                    const raw = await uow.runRaw(`SELECT created_at FROM im_deliveries WHERE id = $1`, ['delivery-1']);
                    assert.equal(new Date(raw[0].created_at).toISOString(), T0);
                },
            );
        });

        await test('a concurrent saveAttempt with a new id converges on the (delivery, attempt) business key', async () => {
            await withUow(
                () => makePostgresUow(),
                async (uow) => {
                    await uow.transaction(async (ctx) => {
                        await ctx.deliveries.save(delivery());
                        await ctx.deliveries.saveAttempt(attempt('attempt-1', 'delivery-1'));
                    });
                    // 模拟并发重试：另一事务用不同 id 写同一 (delivery_id, attempt_no)，应收敛而非报唯一约束冲突
                    await uow.transaction((ctx) =>
                        ctx.deliveries.saveAttempt(attempt('attempt-other', 'delivery-1', { requestId: 'request-2' })),
                    );
                    const found = await uow.transaction((ctx) => ctx.deliveries.findAttempt('delivery-1', 1));
                    assert.equal(found.id, 'attempt-other');
                    assert.equal(found.requestId, 'request-2');
                    const count = await uow.runRaw('SELECT COUNT(*)::int AS n FROM im_delivery_attempts');
                    assert.equal(count[0].n, 1);
                },
            );
        });

        await test('concurrent saveReceipt on the same dedupe_key preserves the first receipt without error', async () => {
            const [first, second] = await Promise.all([makePostgresUow(), makePostgresUow()]);
            try {
                await first.transaction((ctx) => ctx.deliveries.save(delivery()));
                await Promise.all([
                    first.transaction((ctx) => ctx.deliveries.saveReceipt(receipt('receipt-a'))),
                    second.transaction((ctx) =>
                        ctx.deliveries.saveReceipt(receipt('receipt-b', { detail: { deliveredAt: T2 } })),
                    ),
                ]);
                const found = await first.transaction((ctx) => ctx.deliveries.findReceiptByDedupeKey('dedupe-1'));
                assert.notEqual(found, undefined);
                const count = await first.runRaw('SELECT COUNT(*)::int AS n FROM im_delivery_receipts');
                assert.equal(count[0].n, 1);
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('concurrent createIfAbsent on the same business key creates exactly once', async () => {
            const [first, second] = await Promise.all([makePostgresUow(), makePostgresUow()]);
            try {
                const [a, b] = await Promise.all([
                    first.transaction((ctx) => ctx.deliveries.createIfAbsent(delivery('delivery-a'))),
                    second.transaction((ctx) => ctx.deliveries.createIfAbsent(delivery('delivery-b'))),
                ]);
                assert.equal(a.id, b.id);
                assert.equal([a.created, b.created].filter(Boolean).length, 1);
                const count = await first.runRaw('SELECT COUNT(*)::int AS n FROM im_deliveries');
                assert.equal(count[0].n, 1);
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('concurrent action creation converges on one idempotency-key winner', async () => {
            const [first, second] = await Promise.all([makePostgresUow(), makePostgresUow()]);
            try {
                const [a, b] = await Promise.all([
                    first.transaction((ctx) =>
                        ctx.actions.createIfAbsent(
                            action('action-a', { operationId: 'operation-a', actionKeyHash: 'hash-shared' }),
                        ),
                    ),
                    second.transaction((ctx) =>
                        ctx.actions.createIfAbsent(
                            action('action-b', { operationId: 'operation-b', actionKeyHash: 'hash-shared' }),
                        ),
                    ),
                ]);
                assert.equal(a.action.id, b.action.id);
                assert.equal([a.created, b.created].filter(Boolean).length, 1);
                const count = await first.runRaw('SELECT COUNT(*)::int AS n FROM im_actions');
                assert.equal(count[0].n, 1);
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('concurrent claimForDispatch yields exactly one winner', async () => {
            const [first, second] = await Promise.all([makePostgresUow(), makePostgresUow()]);
            try {
                await first.transaction((ctx) => ctx.deliveries.save(delivery()));
                const results = await Promise.all([
                    first.transaction((ctx) => ctx.deliveries.claimForDispatch('delivery-1', T1, 60)),
                    second.transaction((ctx) => ctx.deliveries.claimForDispatch('delivery-1', T1, 60)),
                ]);
                const winners = results.filter((result) => result !== undefined);
                assert.equal(winners.length, 1);
                assert.equal(winners[0].status, 'sending');
                assert.notEqual(winners[0].claimToken, undefined);
                const after = await first.transaction((ctx) => ctx.deliveries.findById('delivery-1'));
                assert.equal(after.status, 'sending');
                assert.notEqual(after.claimToken, undefined);
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('concurrent outbox workers claim a due event exactly once per lease', async () => {
            const [first, second] = await Promise.all([makePostgresUow(), makePostgresUow()]);
            try {
                await first.transaction((context) =>
                    context.outbox.append(
                        outboxEvent('outbox-concurrent', {
                            eventType: 'im.delivery.requested',
                            aggregateId: 'delivery-1',
                        }),
                    ),
                );
                const claims = await Promise.all([
                    first.transaction((context) => context.outbox.claimPending(['im.delivery.requested'], T1, T2, 10)),
                    second.transaction((context) => context.outbox.claimPending(['im.delivery.requested'], T1, T2, 10)),
                ]);
                assert.equal(claims.flat().length, 1);
                assert.equal(claims.flat()[0].id, 'outbox-concurrent');
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('a reclaimed claim fences the stale worker out of later writes', async () => {
            const [first, second] = await Promise.all([makePostgresUow(), makePostgresUow()]);
            try {
                await first.transaction((ctx) => ctx.deliveries.save(delivery()));
                const claimA = await first.transaction((ctx) => ctx.deliveries.claimForDispatch('delivery-1', T0, 60));
                // lease 过期后另一个 worker 重领
                const claimB = await second.transaction((ctx) => ctx.deliveries.claimForDispatch('delivery-1', T2, 60));
                assert.notEqual(claimB, undefined);
                assert.notEqual(claimB.claimToken, claimA.claimToken);
                // 旧 worker A 的迟到终态写被 fenced
                const stale = await first.transaction((ctx) =>
                    ctx.deliveries.saveIfClaimed(
                        delivery('delivery-1', { status: 'retryable_failed', lastErrorCode: 'busy', updatedAt: T2 }),
                        claimA.claimToken,
                    ),
                );
                assert.equal(stale, undefined);
                // 当前 owner B 的终态写生效
                const applied = await second.transaction((ctx) =>
                    ctx.deliveries.saveIfClaimed(
                        delivery('delivery-1', { status: 'accepted', externalMessageId: 'platform-1', updatedAt: T2 }),
                        claimB.claimToken,
                    ),
                );
                assert.equal(applied.status, 'accepted');
                const after = await first.transaction((ctx) => ctx.deliveries.findById('delivery-1'));
                assert.equal(after.status, 'accepted');
                assert.equal(after.externalMessageId, 'platform-1');
                assert.equal(after.claimedAt, undefined);
                assert.equal(after.claimToken, undefined);
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('a delivery left in sending by a crash is reclaimed and completed by a fresh process', async () => {
            const first = new PostgresImUnitOfWork(POSTGRES_URL);
            await first.migrate();
            await first.truncateAll();
            await first.transaction((ctx) => ctx.deliveries.save(delivery()));
            const claim = await first.transaction((ctx) => ctx.deliveries.claimForDispatch('delivery-1', T0, 60));
            const staleToken = claim.claimToken;
            await first.close(); // “崩溃”：claim 行以 sending/claimedAt=T0 留在库中

            const second = new PostgresImUnitOfWork(POSTGRES_URL);
            const reclaimed = await second.transaction((ctx) => ctx.deliveries.claimForDispatch('delivery-1', T2, 60));
            assert.notEqual(reclaimed, undefined);
            // 旧进程的迟到写被 fenced
            const stale = await second.transaction((ctx) =>
                ctx.deliveries.saveIfClaimed(
                    delivery('delivery-1', { status: 'accepted', externalMessageId: 'stale', updatedAt: T2 }),
                    staleToken,
                ),
            );
            assert.equal(stale, undefined);
            const completed = await second.transaction((ctx) =>
                ctx.deliveries.saveIfClaimed(
                    delivery('delivery-1', { status: 'accepted', externalMessageId: 'platform-2', updatedAt: T2 }),
                    reclaimed.claimToken,
                ),
            );
            assert.equal(completed.status, 'accepted');
            assert.equal(completed.externalMessageId, 'platform-2');
            await second.close();
        });

        await test('a legacy sending row with no claim columns is reclaimed after migration', async () => {
            const uow = new PostgresImUnitOfWork(POSTGRES_URL);
            await uow.migrate();
            await uow.truncateAll();
            // 模拟 v1 库：移除 claim 列并回退版本行
            await uow.runRaw('ALTER TABLE im_deliveries DROP COLUMN claimed_at');
            await uow.runRaw('ALTER TABLE im_deliveries DROP COLUMN claim_token');
            await uow.runRaw('DELETE FROM im_schema_migrations WHERE version >= 2');
            await uow.runRaw(
                `INSERT INTO im_deliveries (
                    id, business_event_id, correlation_id, binding_id, channel_account_id, kind,
                    semantic_payload, presentation_type, status, external_message_id, expires_at, last_error_code, created_at, updated_at
                 ) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, 'sending', NULL, NULL, NULL, $9, $9)`,
                [
                    'delivery-legacy',
                    'event-legacy',
                    'corr-legacy',
                    'binding-1',
                    'channel-1',
                    'reminder_due',
                    '{}',
                    'template',
                    T0,
                ],
            );
            // 升级补列后，遗留 sending 行（claimed_at NULL）可重领
            await uow.migrate();
            const reclaimed = await uow.transaction((ctx) =>
                ctx.deliveries.claimForDispatch('delivery-legacy', T2, 60),
            );
            assert.notEqual(reclaimed, undefined);
            assert.equal(reclaimed.status, 'sending');
            assert.equal(reclaimed.claimedAt, T2);
            assert.notEqual(reclaimed.claimToken, undefined);
            await uow.close();
        });

        await test('migration keeps the latest duplicate active binding and prevents it from recurring', async () => {
            const uow = new PostgresImUnitOfWork(POSTGRES_URL);
            await uow.migrate();
            await uow.truncateAll();
            await uow.runRaw('DROP INDEX IF EXISTS im_bindings_active_user_device_identity_uq');
            await uow.runRaw('DROP INDEX IF EXISTS im_bindings_active_device_uq');
            await uow.runRaw('DROP INDEX IF EXISTS im_bindings_active_external_identity_uq');
            await uow.runRaw('ALTER TABLE im_bindings DROP CONSTRAINT IF EXISTS im_bindings_active_device_check');
            await uow.runRaw('DELETE FROM im_schema_migrations WHERE version >= 3');
            await uow.runRaw(
                `INSERT INTO im_bindings (
                    id, user_id, device_id, external_identity_id, priority, status, bound_at, unbound_at, revoked_at
                 ) VALUES
                    ($1, $2, $3, $4, 100, 'active', $5, NULL, NULL),
                    ($6, $2, $3, $4, 100, 'active', $7, NULL, NULL)`,
                [
                    'binding-earlier',
                    'user-duplicate',
                    'device-duplicate',
                    'identity-duplicate',
                    T0,
                    'binding-later',
                    T1,
                ],
            );

            await uow.migrate();

            const bindings = await uow.runRaw(
                `SELECT id, status FROM im_bindings
                 WHERE user_id = $1 ORDER BY id`,
                ['user-duplicate'],
            );
            assert.deepEqual(bindings, [
                { id: 'binding-earlier', status: 'unbound' },
                { id: 'binding-later', status: 'active' },
            ]);
            await assert.rejects(
                uow.runRaw(
                    `INSERT INTO im_bindings (
                        id, user_id, device_id, external_identity_id, priority, status, bound_at, unbound_at, revoked_at
                     ) VALUES ($1, $2, $3, $4, 100, 'active', $5, NULL, NULL)`,
                    ['binding-third', 'user-duplicate', 'device-duplicate', 'identity-duplicate', T2],
                ),
            );
            await uow.close();
        });

        await test('v7 migration cleans null and dual-ranked conflicts then validates one-to-one constraints', async () => {
            const uow = new PostgresImUnitOfWork(POSTGRES_URL);
            await uow.migrate();
            await uow.truncateAll();
            await uow.runRaw('DROP INDEX IF EXISTS im_bindings_active_device_uq');
            await uow.runRaw('DROP INDEX IF EXISTS im_bindings_active_external_identity_uq');
            await uow.runRaw('ALTER TABLE im_bindings DROP CONSTRAINT IF EXISTS im_bindings_active_device_check');
            await uow.runRaw('DELETE FROM im_schema_migrations WHERE version >= 7');
            await uow.runRaw(
                `INSERT INTO im_bindings
                    (id, user_id, device_id, external_identity_id, priority, status, bound_at)
                 VALUES
                    ('binding-null', 'user-1', NULL, 'identity-null', 100, 'active', $1),
                    ('binding-old', 'user-1', 'device-d', 'identity-a', 100, 'active', $1),
                    ('binding-device-winner', 'user-1', 'device-d', 'identity-b', 100, 'active', $3),
                    ('binding-identity-winner', 'user-1', 'device-e', 'identity-a', 100, 'active', $2)`,
                [T0, T1, T2],
            );
            await uow.migrate();
            const rows = await uow.runRaw('SELECT id, status FROM im_bindings ORDER BY id');
            assert.deepEqual(rows, [
                { id: 'binding-device-winner', status: 'active' },
                { id: 'binding-identity-winner', status: 'active' },
                { id: 'binding-null', status: 'unbound' },
                { id: 'binding-old', status: 'unbound' },
            ]);
            const validation = await uow.runRaw(
                `SELECT convalidated FROM pg_constraint WHERE conname = 'im_bindings_active_device_check'`,
            );
            assert.deepEqual(validation, [{ convalidated: true }]);
            const indexes = await uow.runRaw(
                `SELECT indexname FROM pg_indexes
                 WHERE indexname IN ('im_bindings_active_device_uq', 'im_bindings_active_external_identity_uq')
                 ORDER BY indexname`,
            );
            assert.deepEqual(indexes, [
                { indexname: 'im_bindings_active_device_uq' },
                { indexname: 'im_bindings_active_external_identity_uq' },
            ]);
            await assert.rejects(
                uow.runRaw(
                    `INSERT INTO im_bindings
                        (id, user_id, device_id, external_identity_id, priority, status, bound_at)
                     VALUES ('binding-invalid', 'user-1', NULL, 'identity-new', 100, 'active', $1)`,
                    [T2],
                ),
            );
            await uow.close();
        });

        await test('v6 device registry enforces raw 32-byte unique digests and owner lookup index', async () => {
            await withUow(
                () => makePostgresUow(),
                async (uow) => {
                    const columns = await uow.runRaw(
                        `SELECT data_type FROM information_schema.columns
                         WHERE table_name = 'im_devices' AND column_name = 'token_digest'`,
                    );
                    assert.deepEqual(columns, [{ data_type: 'bytea' }]);
                    const indexes = await uow.runRaw(
                        `SELECT indexname FROM pg_indexes WHERE indexname = 'im_devices_user_id_idx'`,
                    );
                    assert.deepEqual(indexes, [{ indexname: 'im_devices_user_id_idx' }]);
                    await assert.rejects(
                        uow.runRaw(
                            `INSERT INTO im_devices
                                (device_id, user_id, token_digest, status, created_at, updated_at)
                             VALUES ('device-short', 'user-1', $1, 'active', $2, $2)`,
                            [Buffer.alloc(31), T0],
                        ),
                    );
                },
            );
        });

        await test('concurrent cross-device identity swap is deadlock-free and leaves one-to-one active rows', async () => {
            const first = await makePostgresUow();
            const second = new PostgresImUnitOfWork(POSTGRES_URL);
            await second.migrate();
            try {
                await first.transaction(async (tx) => {
                    await tx.bindings.replaceActiveBinding({
                        id: 'binding-a',
                        userId: 'user-1',
                        deviceId: 'device-a',
                        externalIdentityId: 'identity-a',
                        priority: 100,
                        status: 'active',
                        boundAt: T0,
                    });
                    await tx.bindings.replaceActiveBinding({
                        id: 'binding-b',
                        userId: 'user-1',
                        deviceId: 'device-b',
                        externalIdentityId: 'identity-b',
                        priority: 100,
                        status: 'active',
                        boundAt: T0,
                    });
                });
                await withTimeout(
                    Promise.all([
                        first.transaction((tx) =>
                            tx.bindings.replaceActiveBinding({
                                id: 'binding-a-to-b',
                                userId: 'user-1',
                                deviceId: 'device-a',
                                externalIdentityId: 'identity-b',
                                priority: 100,
                                status: 'active',
                                boundAt: T1,
                            }),
                        ),
                        second.transaction((tx) =>
                            tx.bindings.replaceActiveBinding({
                                id: 'binding-b-to-a',
                                userId: 'user-1',
                                deviceId: 'device-b',
                                externalIdentityId: 'identity-a',
                                priority: 100,
                                status: 'active',
                                boundAt: T1,
                            }),
                        ),
                    ]),
                    5_000,
                    'binding swap',
                );
                const active = await first.runRaw(
                    `SELECT device_id, external_identity_id FROM im_bindings
                     WHERE status = 'active' ORDER BY device_id`,
                );
                assert.deepEqual(active, [
                    { device_id: 'device-a', external_identity_id: 'identity-b' },
                    { device_id: 'device-b', external_identity_id: 'identity-a' },
                ]);
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('migration keeps one pending display code and prevents a collision from recurring', async () => {
            const uow = new PostgresImUnitOfWork(POSTGRES_URL);
            await uow.migrate();
            await uow.truncateAll();
            await uow.runRaw('DROP INDEX IF EXISTS im_pairing_sessions_pending_display_code_hash_uq');
            await uow.runRaw('DELETE FROM im_schema_migrations WHERE version >= 4');
            await uow.runRaw(
                `INSERT INTO im_pairing_sessions (
                    id, display_code_hash, user_id, device_id, allowed_platforms, status, expires_at, created_at, confirmed_at
                 ) VALUES
                    ($1, $2, $3, $4, NULL, 'pending', $5, $6, NULL),
                    ($7, $2, $3, $8, NULL, 'pending', $5, $9, NULL)`,
                [
                    'pairing-earlier',
                    'hash-duplicate',
                    'user-duplicate',
                    'device-earlier',
                    T2,
                    T0,
                    'pairing-later',
                    'device-later',
                    T1,
                ],
            );

            await uow.migrate();

            const sessions = await uow.runRaw(
                `SELECT id, status FROM im_pairing_sessions
                 WHERE display_code_hash = $1 ORDER BY id`,
                ['hash-duplicate'],
            );
            assert.deepEqual(sessions, [
                { id: 'pairing-earlier', status: 'cancelled' },
                { id: 'pairing-later', status: 'pending' },
            ]);
            await assert.rejects(
                uow.runRaw(
                    `INSERT INTO im_pairing_sessions (
                        id, display_code_hash, user_id, device_id, allowed_platforms, status, expires_at, created_at, confirmed_at
                     ) VALUES ($1, $2, $3, $4, NULL, 'pending', $5, $6, NULL)`,
                    ['pairing-third', 'hash-duplicate', 'user-duplicate', 'device-third', T2, T2],
                ),
            );
            await uow.close();
        });

        await test('concurrent identical notifications yield one delivery and one outbox event', async () => {
            const clock = new FixedClock();
            const [first, second] = await Promise.all([makePostgresUow(), makePostgresUow()]);
            try {
                const gatewayA = createMockImGateway('device-fixture', clock, { unitOfWork: first });
                const gatewayB = createMockImGateway('device-fixture', clock, { unitOfWork: second });
                await bindFixtureUser(gatewayA);
                const intent = strongIntent();
                const [subA, subB] = await Promise.all([
                    gatewayA.application.notifications.submitNotification(intent),
                    gatewayB.application.notifications.submitNotification(intent),
                ]);
                assert.deepEqual(subA, subB);
                const deliveries = await first.runRaw(
                    'SELECT COUNT(*)::int AS n FROM im_deliveries WHERE business_event_id = $1',
                    [intent.businessEventId],
                );
                assert.equal(deliveries[0].n, 1);
                const outbox = await first.runRaw(
                    `SELECT COUNT(*)::int AS n FROM im_outbox_events WHERE event_type = 'im.delivery.requested'`,
                );
                assert.equal(outbox[0].n, 1);
                const submissions = await first.runRaw(
                    'SELECT COUNT(*)::int AS n FROM im_intent_submissions WHERE business_event_id = $1',
                    [intent.businessEventId],
                );
                assert.equal(submissions[0].n, 1);
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('concurrent conflicting notifications surface idempotency_conflict and keep the winner', async () => {
            const clock = new FixedClock();
            const [first, second] = await Promise.all([makePostgresUow(), makePostgresUow()]);
            try {
                const gatewayA = createMockImGateway('device-fixture', clock, { unitOfWork: first });
                const gatewayB = createMockImGateway('device-fixture', clock, { unitOfWork: second });
                await bindFixtureUser(gatewayA);
                const intentA = strongIntent({ content: { title: 'first title' } });
                const intentB = strongIntent({ content: { title: 'second title' } });
                const results = await Promise.allSettled([
                    gatewayA.application.notifications.submitNotification(intentA),
                    gatewayB.application.notifications.submitNotification(intentB),
                ]);
                const fulfilled = results.filter((result) => result.status === 'fulfilled');
                const rejected = results.filter((result) => result.status === 'rejected');
                assert.equal(fulfilled.length, 1);
                assert.equal(rejected.length, 1);
                assert.match(
                    rejected[0].reason.message,
                    /Business event ID was already used with different contract content/,
                );
                const submissions = await first.runRaw(
                    'SELECT COUNT(*)::int AS n FROM im_intent_submissions WHERE business_event_id = $1',
                    [intentA.businessEventId],
                );
                assert.equal(submissions[0].n, 1);
                const stored = await first.transaction((ctx) =>
                    ctx.intentSubmissions.findByBusinessKey(intentA.businessEventId, 'reminder_due'),
                );
                assert.notEqual(stored.requestFingerprint, '');
                assert.deepEqual(stored.submission, fulfilled[0].value);
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('the full gateway action loop survives a restart against Postgres', async () => {
            const first = new PostgresImUnitOfWork(POSTGRES_URL);
            await first.migrate();
            await first.truncateAll();
            const clock = new FixedClock();
            const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: first });
            await bindFixtureUser(gateway);
            const submission = await gateway.application.notifications.submitNotification(strongIntent());
            const deliveryId = submission.deliveries[0].deliveryId;
            await gateway.application.deliveryDispatch.dispatch(deliveryId);
            const token = await gateway.application.actionUi.issue(deliveryId);
            const command = await gateway.application.actionUi.execute({ token, action: 'acknowledge' });
            await first.close();

            const second = new PostgresImUnitOfWork(POSTGRES_URL);
            const replayedGateway = createMockImGateway('device-fixture', clock, { unitOfWork: second });
            const replay = await replayedGateway.application.actions.replayPending(
                command.deviceId,
                command.reminderTriggerId,
            );
            assert.equal(replay.length, 1);
            assert.equal(replay[0].commandId, command.commandId);
            assert.equal(replay[0].operationId, command.operationId);
            await second.close();
        });

        await test('a fresh delivery worker dispatches an outbox request persisted before restart', async () => {
            const clock = new FixedClock();
            const first = await makePostgresUow();
            const gatewayA = createMockImGateway('device-fixture', clock, { unitOfWork: first });
            await bindFixtureUser(gatewayA);
            const submission = await gatewayA.application.notifications.submitNotification(weakIntent());
            const deliveryId = submission.deliveries[0].deliveryId;
            await first.close();

            const second = new PostgresImUnitOfWork(POSTGRES_URL);
            try {
                const gatewayB = createMockImGateway('device-fixture', clock, { unitOfWork: second });
                const worker = new DeliveryOutboxWorker({
                    unitOfWork: second,
                    dispatch: gatewayB.application.deliveryDispatch,
                    clock,
                    logger: { log: () => {} },
                });
                assert.equal(await worker.runOnce(), 1);
                const recovered = await gatewayB.application.deliveries.find(deliveryId);
                assert.equal(recovered.delivery.status, 'accepted');
                const outbox = await second.runRaw(
                    `SELECT status FROM im_outbox_events WHERE aggregate_id = $1 ORDER BY created_at`,
                    [deliveryId],
                );
                assert.deepEqual(
                    outbox.map((event) => event.status),
                    ['published'],
                );
            } finally {
                await second.close();
            }
        });

        await test('a restart through the production composition root recovers pending actions', async () => {
            const reset = new PostgresImUnitOfWork(POSTGRES_URL);
            await reset.migrate();
            await reset.truncateAll();
            await reset.transaction((tx) =>
                tx.devices.create({
                    deviceId: 'device-fixture',
                    userId: 'user-fixture',
                    tokenDigest: new Uint8Array(32),
                    status: 'active',
                    createdAt: T0,
                    updatedAt: T0,
                }),
            );
            await reset.close();

            const clock = new FixedClock();
            const first = await createPostgresImGateway({
                databaseUrl: POSTGRES_URL,
                ports: mockImGatewayPorts('device-fixture', clock),
            });
            await bindFixtureUser(first.runtime);
            const submission = await first.runtime.application.notifications.submitNotification(strongIntent());
            const deliveryId = submission.deliveries[0].deliveryId;
            await first.runtime.application.deliveryDispatch.dispatch(deliveryId);
            const token = await first.runtime.application.actionUi.issue(deliveryId);
            const command = await first.runtime.application.actionUi.execute({ token, action: 'acknowledge' });
            await first.close();

            const second = await createPostgresImGateway({
                databaseUrl: POSTGRES_URL,
                ports: mockImGatewayPorts('device-fixture', clock),
            });
            const replay = await second.runtime.application.actions.replayPending(
                command.deviceId,
                command.reminderTriggerId,
            );
            assert.equal(replay.length, 1);
            assert.equal(replay[0].commandId, command.commandId);
            assert.equal(replay[0].operationId, command.operationId);
            await second.close();
        });

        await test('migrate records every schema version row and is idempotent', async () => {
            const uow = new PostgresImUnitOfWork(POSTGRES_URL);
            await uow.migrate();
            await uow.truncateAll();
            const before = await uow.runRaw('SELECT version FROM im_schema_migrations ORDER BY version');
            assert.deepEqual(
                before,
                Array.from({ length: SCHEMA_VERSION }, (_, i) => ({ version: i + 1 })),
            );
            await uow.migrate();
            const after = await uow.runRaw('SELECT COUNT(*)::int AS n FROM im_schema_migrations');
            assert.equal(after[0].n, SCHEMA_VERSION);
            await uow.close();
        });

        await test('concurrent confirm and revoke linearize without a reverse lock deadlock', async () => {
            const clock = new FixedClock();
            const [first, second] = await Promise.all([makePostgresUow(), makePostgresUow()]);
            try {
                const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: first });
                const channel = await gateway.application.channels.register({
                    platform: 'wechat_official',
                    tenantExternalId: 'tenant-race',
                    koishiBotId: 'bot-race',
                    credentialRef: 'secret://race',
                    connectionMode: 'webhook',
                });
                const pairing = await gateway.application.pairing.create({
                    userId: 'user-fixture',
                    deviceId: 'device-fixture',
                });
                const management = new DeviceManagementService(second, clock);
                const [confirmation, revocation] = await withTimeout(
                    Promise.allSettled([
                        gateway.application.pairing.confirm({
                            displayCode: pairing.displayCode,
                            channelAccountId: channel.id,
                            externalUserId: 'race-open-id',
                        }),
                        management.revoke('device-fixture'),
                    ]),
                    5_000,
                    'confirm/revoke race',
                );
                assert.equal(revocation.status, 'fulfilled');
                const state = await first.runRaw(
                    `SELECT d.status AS device_status, p.status AS pairing_status,
                            (SELECT COUNT(*)::int FROM im_bindings WHERE status = 'active') AS active_bindings
                     FROM im_devices d JOIN im_pairing_sessions p ON p.device_id = d.device_id
                     WHERE d.device_id = 'device-fixture'`,
                );
                assert.equal(state[0].device_status, 'revoked');
                if (confirmation.status === 'fulfilled') {
                    assert.deepEqual(state[0], {
                        device_status: 'revoked',
                        pairing_status: 'confirmed',
                        active_bindings: 1,
                    });
                } else {
                    assert.deepEqual(state[0], {
                        device_status: 'revoked',
                        pairing_status: 'cancelled',
                        active_bindings: 0,
                    });
                }
            } finally {
                await Promise.all([first.close(), second.close()]);
            }
        });

        await test('concurrent migrate calls serialize on the advisory lock without error', async () => {
            const first = new PostgresImUnitOfWork(POSTGRES_URL);
            const second = new PostgresImUnitOfWork(POSTGRES_URL);
            await Promise.all([first.migrate(), second.migrate()]);
            const rows = await first.runRaw('SELECT COUNT(*)::int AS n FROM im_schema_migrations');
            assert.equal(rows[0].n, SCHEMA_VERSION);
            await Promise.all([first.close(), second.close()]);
        });

        await test('a failing migration rolls back the whole batch leaving no partial schema', async () => {
            const uow = new PostgresImUnitOfWork(POSTGRES_URL);
            await uow.migrate();
            // 清空业务表与版本表，从“零”验证失败批次的原子回滚
            const dropList = [...IM_TABLES].reverse().concat('im_schema_migrations').join(', ');
            await uow.runRaw(`DROP TABLE IF EXISTS ${dropList} CASCADE`);
            // 预置一个缺列的配对会话表，令迁移在建索引处失败
            await uow.runRaw('CREATE TABLE im_pairing_sessions (id text PRIMARY KEY)');
            await assert.rejects(uow.migrate(), 'The sabotaged migration did not reject');
            const tables = await uow.runRaw(
                `SELECT table_name FROM information_schema.tables
                 WHERE table_schema = 'public' AND table_name LIKE 'im_%' ORDER BY table_name`,
            );
            assert.deepEqual(
                tables.map((row) => row.table_name),
                ['im_pairing_sessions', 'im_schema_migrations'],
                '除预置表与版本表引导外，其余表都应随事务回滚',
            );
            const versions = await uow.runRaw('SELECT COUNT(*)::int AS n FROM im_schema_migrations');
            assert.equal(versions[0].n, 0, '失败的批次不应记录版本');
            // 修复后重跑迁移应完整成功
            await uow.runRaw('DROP TABLE im_pairing_sessions');
            await uow.migrate();
            const fixed = await uow.runRaw('SELECT version FROM im_schema_migrations ORDER BY version');
            assert.deepEqual(
                fixed,
                Array.from({ length: SCHEMA_VERSION }, (_, i) => ({ version: i + 1 })),
            );
            const channel = await uow.runRaw('SELECT COUNT(*)::int AS n FROM im_channel_accounts');
            assert.equal(channel[0].n, 0);
            await uow.close();
        });
    },
);
