import { describe, test } from 'node:test';
import assert from 'node:assert/strict';

import { createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';
import { bindFixtureUser, strongIntent } from './helpers.mjs';

/** 默认连接地址与 docker-compose.yml 保持一致；CI 通过 DATABASE_URL 覆盖。 */
const POSTGRES_URL = process.env.DATABASE_URL ?? 'postgres://voicelife:voicelife@localhost:5432/voicelife';

const T0 = '2026-08-03T00:00:00.000Z';
const T1 = '2026-08-03T00:10:00.000Z';
const T2 = '2026-08-03T00:20:00.000Z';
const LATE = '2026-08-03T01:00:00.000Z';

/** 构造渠道账号聚合。 */
function channelAccount(id = 'channel-1', overrides = {}) {
    return {
        id,
        platform: 'wechat_official',
        tenantExternalId: 'tenant-a',
        koishiBotId: 'bot-a',
        credentialRef: 'secret://a',
        connectionMode: 'webhook',
        capabilityConfig: { richCard: true },
        status: 'active',
        createdAt: T0,
        updatedAt: T0,
        ...overrides,
    };
}

/** 构造配对会话聚合。 */
function pairingSession(id = 'pairing-1', overrides = {}) {
    return {
        id,
        displayCodeHash: 'hash-1234',
        userId: 'user-1',
        deviceId: 'device-1',
        allowedPlatforms: ['wechat_official'],
        status: 'pending',
        expiresAt: LATE,
        createdAt: T0,
        ...overrides,
    };
}

/** 构造受保护外部身份聚合。 */
function externalIdentity(id = 'identity-1', overrides = {}) {
    return {
        id,
        channelAccountId: 'channel-1',
        externalUserIdCiphertext: 'cipher-open-id',
        externalUserIdHash: 'hash-open-id',
        displayName: 'Alice',
        status: 'active',
        createdAt: T0,
        updatedAt: T0,
        ...overrides,
    };
}

/** 构造用户与外部身份绑定聚合。 */
function binding(id = 'binding-1', overrides = {}) {
    return {
        id,
        userId: 'user-1',
        deviceId: 'device-1',
        externalIdentityId: 'identity-1',
        priority: 10,
        status: 'active',
        boundAt: T0,
        ...overrides,
    };
}

/** 构造规范化入站事件记录。 */
function inboundEvent(id = 'inbound-1', overrides = {}) {
    return {
        id,
        channelAccountId: 'channel-1',
        externalEventId: 'external-1',
        eventType: 'message.received',
        payload: { text: 'hello' },
        status: 'received',
        occurredAt: T0,
        receivedAt: T0,
        ...overrides,
    };
}

/** 构造请求级幂等受理记录。 */
function intentSubmission(businessEventId = 'event-1', overrides = {}) {
    return {
        businessEventId,
        kind: 'reminder_due',
        requestFingerprint: 'fingerprint-1',
        submission: {
            businessEventId,
            status: 'accepted',
            deliveries: [{ deliveryId: 'delivery-1', bindingId: 'binding-1', status: 'pending' }],
        },
        createdAt: T0,
        ...overrides,
    };
}

/** 构造一次消息投递。 */
function delivery(id = 'delivery-1', overrides = {}) {
    return {
        id,
        businessEventId: 'event-1',
        correlationId: 'correlation-1',
        bindingId: 'binding-1',
        channelAccountId: 'channel-1',
        kind: 'reminder_due',
        semanticPayload: {
            businessEventId: 'event-1',
            reminderType: 'strong',
            reminderTriggerId: 'trigger-1',
            recipient: { userId: 'user-1', deviceId: 'device-1' },
        },
        presentationType: 'template',
        status: 'pending',
        expiresAt: T2,
        createdAt: T0,
        updatedAt: T0,
        ...overrides,
    };
}

/** 构造一次发送尝试。 */
function attempt(id = 'attempt-1', deliveryId = 'delivery-1', overrides = {}) {
    return {
        id,
        deliveryId,
        attemptNo: 1,
        requestId: 'request-1',
        renderedPayload: { title: 'reminder' },
        status: 'accepted',
        platformMessageId: 'platform-msg-1',
        startedAt: T0,
        completedAt: T1,
        ...overrides,
    };
}

/** 构造平台投递回执。 */
function receipt(id = 'receipt-1', overrides = {}) {
    return {
        id,
        deliveryId: 'delivery-1',
        attemptId: 'attempt-1',
        stage: 'delivered',
        dedupeKey: 'dedupe-1',
        externalEventId: 'platform-receipt-1',
        detail: { deliveredAt: T1 },
        occurredAt: T1,
        receivedAt: T1,
        ...overrides,
    };
}

/** 构造提醒动作。 */
function action(id = 'action-1', overrides = {}) {
    return {
        id,
        operationId: 'operation-1',
        correlationId: 'correlation-1',
        deliveryId: 'delivery-1',
        actorBindingId: 'binding-1',
        deviceId: 'device-1',
        reminderTriggerId: 'trigger-1',
        actionType: 'snooze',
        actionParams: { minutes: 10 },
        actionKeyHash: 'hash-action-1',
        expectedIdentityId: 'identity-1',
        status: 'pending',
        expiresAt: T2,
        createdAt: T0,
        updatedAt: T0,
        ...overrides,
    };
}

/** 构造事务性发件箱事件。 */
function outboxEvent(id = 'outbox-1', overrides = {}) {
    return {
        id,
        eventType: 'delivery.created',
        aggregateId: 'delivery-1',
        payload: { deliveryId: 'delivery-1' },
        status: 'pending',
        attempts: 0,
        availableAt: T0,
        createdAt: T0,
        ...overrides,
    };
}

/** 在事务内执行工作并安全关闭工作单元。 */
async function withUow(makeUow, fn) {
    const uow = await makeUow();
    try {
        await fn(uow);
    } finally {
        if (typeof uow.close === 'function') await uow.close();
    }
}

/** 构造可直接使用上下文的内存工作单元。 */
function makeInMemoryUow() {
    return new InMemoryImUnitOfWork();
}

/** 构造已迁移并清空表的 Postgres 工作单元。 */
async function makePostgresUow(url = POSTGRES_URL) {
    const uow = new PostgresImUnitOfWork(url);
    await uow.migrate();
    await uow.truncateAll();
    return uow;
}

/** 与内存实现共享的同一套持久化契约断言。 */
async function sharedRepositoryContractSuite(makeUow) {
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
