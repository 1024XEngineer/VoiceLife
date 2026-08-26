import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';

/** 默认连接地址与 docker-compose.yml 保持一致；CI 通过 DATABASE_URL 覆盖。 */
export const POSTGRES_URL = process.env.DATABASE_URL ?? 'postgres://voicelife:voicelife@localhost:5432/voicelife';

export const T0 = '2026-08-03T00:00:00.000Z';
export const T1 = '2026-08-03T00:10:00.000Z';
export const T2 = '2026-08-03T00:20:00.000Z';
export const LATE = '2026-08-03T01:00:00.000Z';

/** 构造注册设备聚合。 */
export function device(deviceId = 'device-1', overrides = {}) {
    return {
        deviceId,
        userId: 'user-1',
        tokenDigest: new Uint8Array(32).fill(7),
        status: 'active',
        createdAt: T0,
        updatedAt: T0,
        ...overrides,
    };
}

/** 构造渠道账号聚合。 */
export function channelAccount(id = 'channel-1', overrides = {}) {
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
export function pairingSession(id = 'pairing-1', overrides = {}) {
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
export function externalIdentity(id = 'identity-1', overrides = {}) {
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
export function binding(id = 'binding-1', overrides = {}) {
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
export function inboundEvent(id = 'inbound-1', overrides = {}) {
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
export function intentSubmission(businessEventId = 'event-1', overrides = {}) {
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
export function delivery(id = 'delivery-1', overrides = {}) {
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
export function attempt(id = 'attempt-1', deliveryId = 'delivery-1', overrides = {}) {
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
export function receipt(id = 'receipt-1', overrides = {}) {
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
export function action(id = 'action-1', overrides = {}) {
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

/** 构造设备语音直接消费后上报的动作事实。 */
export function reminderActionFact(eventId = 'voice-event-1', overrides = {}) {
    return {
        eventId,
        fingerprint: `fingerprint:${eventId}`,
        report: {
            schemaVersion: '1',
            eventId,
            correlationId: 'voice-correlation-1',
            deviceId: 'device-1',
            reminderTriggerId: 'trigger-1',
            operationId: 'voice-operation-1',
            action: 'snooze',
            status: 'succeeded',
            occurredAt: T1,
            nextTriggerAt: T2,
            source: 'voice',
        },
        receivedAt: T1,
        ...overrides,
    };
}

/** 构造事务性发件箱事件。 */
export function outboxEvent(id = 'outbox-1', overrides = {}) {
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
export async function withUow(makeUow, fn) {
    const uow = await makeUow();
    try {
        await fn(uow);
    } finally {
        if (typeof uow.close === 'function') await uow.close();
    }
}

/** 构造可直接使用上下文的内存工作单元。 */
export function makeInMemoryUow() {
    return new InMemoryImUnitOfWork();
}

/** 构造已迁移并清空表的 Postgres 工作单元。 */
export async function makePostgresUow(url = POSTGRES_URL) {
    const uow = new PostgresImUnitOfWork(url);
    await uow.migrate();
    await uow.truncateAll();
    return uow;
}
