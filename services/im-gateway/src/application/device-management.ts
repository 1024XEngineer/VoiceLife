import { createHash, randomBytes, randomUUID } from 'node:crypto';

import { unsafeId, type DeviceId, type UserId } from '../contracts/ids.js';
import type { ImDevice } from '../domain/models.js';
import type { Clock } from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';

const MAX_TOKEN_ATTEMPTS = 8;
const DEVICE_ID_PATTERN = /^[\x21-\x7E]{1,128}$/u;

/** 设备管理命令映射到固定退出码的失败分类。 */
export type DeviceManagementFailure = 'not_found' | 'conflict' | 'internal';

/** 不携带凭据的设备管理业务错误。 */
export class DeviceManagementError extends Error {
    /**
     * @param kind 失败分类。
     * @param message 脱敏消息。
     */
    public constructor(
        public readonly kind: DeviceManagementFailure,
        message: string,
    ) {
        super(message);
        this.name = 'DeviceManagementError';
    }
}

/** 可安全输出且不含 Token 摘要的设备摘要。 */
export interface DeviceSummary {
    readonly deviceId: DeviceId;
    readonly userId: UserId;
    readonly status: ImDevice['status'];
    readonly createdAt: ImDevice['createdAt'];
    readonly updatedAt: ImDevice['updatedAt'];
}

/** 受控 CLI 使用的设备注册、轮换与吊销事务。 */
export class DeviceManagementService {
    /**
     * @param unitOfWork 事务工作单元。
     * @param clock 系统时钟。
     * @param issueToken 256 位 Token 来源。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly clock: Clock,
        private readonly issueToken: () => string = () => randomBytes(32).toString('base64url'),
    ) {}

    /**
     * 注册 active 设备。
     * @param userIdValue 不可变所有者。
     * @param deviceIdValue 可选手工设备标识。
     * @returns 一次性含明文 Token 的创建结果。
     */
    public async create(
        userIdValue: string,
        deviceIdValue = randomUUID(),
    ): Promise<DeviceSummary & { deviceToken: string }> {
        const userId = validateUserId(userIdValue);
        const deviceId = validateDeviceId(deviceIdValue);
        for (let attempt = 0; attempt < MAX_TOKEN_ATTEMPTS; attempt += 1) {
            const deviceToken = this.issueToken();
            assertIssuedToken(deviceToken);
            const now = this.clock.now();
            try {
                await this.unitOfWork.transaction((tx) =>
                    tx.devices.create({
                        deviceId,
                        userId,
                        tokenDigest: tokenDigest(deviceToken),
                        status: 'active',
                        createdAt: now,
                        updatedAt: now,
                    }),
                );
                return { deviceId, userId, deviceToken, status: 'active', createdAt: now, updatedAt: now };
            } catch (error) {
                if (postgresConstraint(error) === 'im_devices_pkey') {
                    throw new DeviceManagementError('conflict', 'Device already exists');
                }
                if (
                    postgresConstraint(error) === 'im_devices_token_digest_key' ||
                    (error instanceof Error && error.message === 'Device already exists')
                ) {
                    if (attempt + 1 < MAX_TOKEN_ATTEMPTS) continue;
                    throw new DeviceManagementError('internal', 'Could not issue a unique device token');
                }
                throw error;
            }
        }
        throw new DeviceManagementError('internal', 'Could not issue a unique device token');
    }

    /**
     * 列出安全设备摘要。
     * @param userIdValue 可选所有者过滤。
     * @returns 不含凭据的列表。
     */
    public list(userIdValue?: string): Promise<readonly DeviceSummary[]> {
        const userId = userIdValue === undefined ? undefined : validateUserId(userIdValue);
        return this.unitOfWork.transaction(async (tx) => (await tx.devices.list(userId)).map(summary));
    }

    /**
     * 为 active 设备轮换 Token。
     * @param deviceIdValue 设备标识。
     * @returns 一次性含新 Token 的结果。
     */
    public async rotateToken(deviceIdValue: string): Promise<DeviceSummary & { deviceToken: string }> {
        const deviceId = validateDeviceId(deviceIdValue);
        for (let attempt = 0; attempt < MAX_TOKEN_ATTEMPTS; attempt += 1) {
            const deviceToken = this.issueToken();
            assertIssuedToken(deviceToken);
            try {
                return await this.unitOfWork.transaction(async (tx) => {
                    const device = await tx.devices.lockById(deviceId);
                    if (device === undefined) throw new DeviceManagementError('not_found', 'Device was not found');
                    if (device.status !== 'active')
                        throw new DeviceManagementError('conflict', 'Revoked device cannot rotate token');
                    const updated = { ...device, tokenDigest: tokenDigest(deviceToken), updatedAt: this.clock.now() };
                    await tx.devices.save(updated);
                    return { ...summary(updated), deviceToken };
                });
            } catch (error) {
                if (postgresConstraint(error) === 'im_devices_token_digest_key') {
                    if (attempt + 1 < MAX_TOKEN_ATTEMPTS) continue;
                    throw new DeviceManagementError('internal', 'Could not issue a unique device token');
                }
                throw error;
            }
        }
        throw new DeviceManagementError('internal', 'Could not issue a unique device token');
    }

    /**
     * 幂等吊销设备并取消 pending 会话。
     * @param deviceIdValue 设备标识。
     * @returns 吊销后的安全摘要。
     */
    public revoke(deviceIdValue: string): Promise<DeviceSummary> {
        const deviceId = validateDeviceId(deviceIdValue);
        return this.unitOfWork.transaction(async (tx) => {
            const device = await tx.devices.lockById(deviceId);
            if (device === undefined) throw new DeviceManagementError('not_found', 'Device was not found');
            if (device.status === 'revoked') return summary(device);
            const revoked = { ...device, status: 'revoked' as const, updatedAt: this.clock.now() };
            await tx.devices.save(revoked);
            await tx.pairingSessions.cancelPendingByDevice(deviceId);
            return summary(revoked);
        });
    }
}

/**
 * 校验手工设备标识。
 * @param value 原始值。
 * @returns 品牌化设备标识。
 */
export function validateDeviceId(value: string): DeviceId {
    if (!DEVICE_ID_PATTERN.test(value))
        throw new TypeError('device-id must be 1..128 printable ASCII bytes without whitespace');
    return unsafeId<DeviceId>(value);
}

/**
 * 校验 CLI 用户标识。
 * @param value 原始值。
 * @returns 品牌化用户标识。
 */
export function validateUserId(value: string): UserId {
    const length = Buffer.byteLength(value, 'utf8');
    if (length < 1 || length > 128 || /\p{Cc}/u.test(value)) {
        throw new TypeError('user-id must be 1..128 UTF-8 bytes without control characters');
    }
    return unsafeId<UserId>(value);
}

/**
 * 计算设备 Token 的 SHA-256 原始摘要。
 * @param token 明文 Token。
 * @returns 32 字节摘要。
 */
export function tokenDigest(token: string): Uint8Array {
    return createHash('sha256').update(token, 'ascii').digest();
}

function assertIssuedToken(token: string): void {
    if (!/^[A-Za-z0-9_-]{43}$/u.test(token)) throw new Error('Token generator returned an invalid token');
}

function summary(device: ImDevice): DeviceSummary {
    return {
        deviceId: device.deviceId,
        userId: device.userId,
        status: device.status,
        createdAt: device.createdAt,
        updatedAt: device.updatedAt,
    };
}

function postgresConstraint(error: unknown): string | undefined {
    if (typeof error !== 'object' || error === null || !('constraint' in error)) return undefined;
    return typeof error.constraint === 'string' ? error.constraint : undefined;
}
