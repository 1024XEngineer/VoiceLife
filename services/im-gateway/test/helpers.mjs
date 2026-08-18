import { readFile } from 'node:fs/promises';

import { ImGatewayError, createMockImGateway } from '../dist/index.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';

const fixtureRoot = new URL('../../../contracts/im-gateway/v1/fixtures/', import.meta.url);

/** 读取共享跨端契约 fixture。 */
export async function readFixture(name) {
    return JSON.parse(await readFile(new URL(name, fixtureRoot), 'utf8'));
}

/** 断言工作函数抛出指定错误码的 ImGatewayError。 */
export async function expectGatewayError(work, code, message) {
    try {
        await work();
    } catch (error) {
        if (error instanceof ImGatewayError && error.code === code) return;
        throw error;
    }
    throw new Error(message);
}

/** 断言工作函数拒绝并返回捕获到的错误。 */
export async function expectRejected(work, message) {
    try {
        await work();
    } catch (error) {
        return error;
    }
    throw new Error(message);
}

/** 构建内存版 Gateway 运行时与确定性时钟。 */
export function buildGateway(overrides = {}) {
    const clock = new FixedClock();
    const unitOfWork = overrides.unitOfWork ?? new InMemoryImUnitOfWork();
    const gateway = createMockImGateway('device-fixture', clock, { ...overrides, unitOfWork });
    return { gateway, clock, unitOfWork };
}

/** 在内存 fixture 中预置匹配所有者的 active 设备。 */
export function seedDevice(unitOfWork, deviceId, userId = 'user-fixture', fill = 1) {
    unitOfWork.seedDevice({
        deviceId,
        userId,
        tokenDigest: new Uint8Array(32).fill(fill),
        status: 'active',
        createdAt: '2026-08-03T00:00:00.000Z',
        updatedAt: '2026-08-03T00:00:00.000Z',
    });
}

/** 注册一个微信公众号渠道账号。 */
export async function registerChannel(gateway, options = {}) {
    return gateway.application.channels.register({
        platform: options.platform ?? 'wechat_official',
        tenantExternalId: options.tenantExternalId ?? 'fixture-account',
        koishiBotId: options.koishiBotId ?? 'fixture-bot',
        credentialRef: options.credentialRef ?? 'secret://fixture-account',
        connectionMode: options.connectionMode ?? 'webhook',
    });
}

/** 注册渠道并完成 user × device × 外部身份配对，返回渠道与绑定。 */
export async function bindFixtureUser(gateway, options = {}) {
    const userId = options.userId ?? 'user-fixture';
    const deviceId = options.deviceId ?? 'device-fixture';
    const externalUserId = options.externalUserId ?? 'fixture-open-id';
    const channel = await registerChannel(gateway, options);
    const session = await gateway.application.pairing.create({ userId, deviceId });
    const binding = await gateway.application.pairing.confirm({
        displayCode: session.displayCode,
        channelAccountId: channel.id,
        externalUserId,
    });
    return { channel, binding };
}

/** 构造合法强提醒通知意图。 */
export function strongIntent(overrides = {}) {
    return {
        schemaVersion: '1',
        businessEventId: 'event-fixture',
        correlationId: 'correlation-fixture',
        kind: 'reminder_due',
        recipient: { userId: 'user-fixture', deviceId: 'device-fixture' },
        scheduleId: 'schedule-fixture',
        taskId: 'task-fixture',
        instanceId: 'instance-fixture',
        reminderTriggerId: 'trigger-fixture',
        reminderType: 'strong',
        content: { title: 'Fixture reminder' },
        plannedAt: '2026-08-03T00:00:00.000Z',
        triggerAt: '2026-08-03T00:00:00.000Z',
        actions: [
            { kind: 'command', type: 'acknowledge', label: '知道了' },
            { kind: 'command', type: 'snooze', label: '推迟 10 分钟', params: { minutes: 10 } },
        ],
        occurredAt: '2026-08-03T00:00:00.000Z',
        ...overrides,
    };
}

/** 构造合法弱提醒通知意图（独立事件标识避免与强提醒幂等冲突）。 */
export function weakIntent(overrides = {}) {
    return {
        ...strongIntent({
            businessEventId: 'event-weak-fixture',
            correlationId: 'correlation-weak-fixture',
            taskId: 'task-weak-fixture',
            instanceId: 'instance-weak-fixture',
            reminderTriggerId: 'trigger-weak-fixture',
            reminderType: 'weak',
            actions: [],
        }),
        ...overrides,
    };
}

/** 构造日程回执意图。 */
export function scheduleReceiptIntent(overrides = {}) {
    return {
        schemaVersion: '1',
        eventId: 'schedule-event-fixture',
        correlationId: 'correlation-schedule-fixture',
        userId: 'user-fixture',
        deviceId: 'device-fixture',
        operationType: 'created',
        scheduleId: 'schedule-fixture',
        result: 'succeeded',
        summary: 'created a schedule',
        occurredAt: '2026-08-03T00:00:00.000Z',
        ...overrides,
    };
}

/** 注册渠道、绑定用户并提交一条强提醒,返回首个投递标识。 */
export async function pendingStrongDelivery(gateway) {
    await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    return submission.deliveries[0].deliveryId;
}

/** 构造返回固定能力集合的渠道能力解析器。 */
export function fixedCapabilities(caps) {
    return {
        resolve: async () => caps,
    };
}
