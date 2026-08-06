import { describe, test } from 'node:test';
import assert from 'node:assert/strict';

import { createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';
import { bindFixtureUser, strongIntent } from './helpers.mjs';
import {
    action,
    attempt,
    channelAccount,
    delivery,
    makeInMemoryUow,
    makePostgresUow,
    outboxEvent,
    receipt,
    POSTGRES_URL,
    T0,
    T1,
    T2,
    withUow,
} from './persistence-fixtures.mjs';
import { sharedRepositoryContractSuite } from './persistence-contract-suite.mjs';

/** 检测 Postgres 是否可用，不可用时返回 null 并跳过整套 Postgres 契约。 */
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

        await test('a concurrent saveReceipt with a new id converges on the dedupe_key', async () => {
            await withUow(
                () => makePostgresUow(),
                async (uow) => {
                    await uow.transaction(async (ctx) => {
                        await ctx.deliveries.save(delivery());
                        await ctx.deliveries.saveReceipt(receipt('receipt-1'));
                    });
                    // 模拟平台 webhook 重复投递：不同 id + 相同 dedupe_key，应收敛而非报唯一约束冲突
                    await uow.transaction((ctx) =>
                        ctx.deliveries.saveReceipt(
                            receipt('receipt-other', { externalEventId: 'platform-receipt-other' }),
                        ),
                    );
                    const found = await uow.transaction((ctx) => ctx.deliveries.findReceiptByDedupeKey('dedupe-1'));
                    assert.equal(found.id, 'receipt-other');
                    const count = await uow.runRaw('SELECT COUNT(*)::int AS n FROM im_delivery_receipts');
                    assert.equal(count[0].n, 1);
                },
            );
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
    },
);
