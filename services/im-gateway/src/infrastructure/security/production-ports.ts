import {
    createCipheriv,
    createDecipheriv,
    createHash,
    createHmac,
    randomInt,
    randomBytes,
    randomUUID,
    timingSafeEqual,
} from 'node:crypto';

import { unsafeId } from '../../contracts/ids.js';
import type {
    ActionId,
    BindingId,
    ChannelAccountId,
    DeliveryAttemptId,
    DeliveryId,
    DeliveryReceiptId,
    DeviceId,
    ExternalIdentityId,
    OperationId,
    OutboxEventId,
    PairingSessionId,
    RequestId,
} from '../../contracts/ids.js';
import type {
    DeviceAuthenticationPort,
    DevicePrincipal,
    ExternalIdentityProtector,
    IdGenerator,
    PairingCodePort,
    ProtectedExternalIdentity,
} from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';

const AES_IV_BYTES = 12;
const AES_TAG_BYTES = 16;
const MINIMUM_SECRET_BYTES = 32;
const TOKEN_PREFIX = 'Bearer ';

/** 使用部署令牌认证单台设备的生产端口。 */
export class BearerDeviceAuthenticationPort implements DeviceAuthenticationPort {
    private readonly expectedTokenDigest: Buffer;

    /**
     * @param deviceId 部署实例接受的设备标识。
     * @param token 由 Secret 注入的 Bearer 令牌。
     */
    public constructor(
        private readonly deviceId: DeviceId,
        token: string,
    ) {
        assertMinimumSecret(token, 'DEVICE_TOKEN', 24);
        this.expectedTokenDigest = digest(token);
    }

    /** {@inheritDoc DeviceAuthenticationPort.authenticate} */
    public authenticate(authorization: string): Promise<DevicePrincipal> {
        if (!authorization.startsWith(TOKEN_PREFIX)) return Promise.reject(unauthorized());
        const actual = digest(authorization.slice(TOKEN_PREFIX.length));
        if (!timingSafeEqual(actual, this.expectedTokenDigest)) return Promise.reject(unauthorized());
        return Promise.resolve({ deviceId: this.deviceId });
    }
}

/** 使用 AES-256-GCM 保护外部身份，并提供仅供出站适配器使用的短时解密。 */
export class AesGcmExternalIdentityProtector implements ExternalIdentityProtector {
    private readonly encryptionKey: Buffer;

    private readonly hashKey: Buffer;

    /** @param secret 由 Secret 注入的身份保护主密钥。 */
    public constructor(secret: string) {
        assertMinimumSecret(secret, 'IDENTITY_SECRET');
        this.encryptionKey = deriveKey(secret, 'identity-encryption');
        this.hashKey = deriveKey(secret, 'identity-hash');
    }

    /** {@inheritDoc ExternalIdentityProtector.protect} */
    public protect(plainExternalUserId: string): Promise<ProtectedExternalIdentity> {
        if (plainExternalUserId.trim() === '') {
            return Promise.reject(new ImGatewayError('invalid_contract', 'External identity is empty'));
        }
        const iv = randomBytes(AES_IV_BYTES);
        const cipher = createCipheriv('aes-256-gcm', this.encryptionKey, iv);
        const ciphertext = Buffer.concat([cipher.update(plainExternalUserId, 'utf8'), cipher.final()]);
        const tag = cipher.getAuthTag();
        return Promise.resolve({
            ciphertext: [
                'v1',
                iv.toString('base64url'),
                ciphertext.toString('base64url'),
                tag.toString('base64url'),
            ].join('.'),
            hash: createHmac('sha256', this.hashKey).update(plainExternalUserId, 'utf8').digest('base64url'),
        });
    }

    /**
     * 解密仅供当前出站请求使用的外部身份。
     * @param protectedValue 数据库保存的受保护身份。
     * @returns 原始平台用户标识。
     */
    public async reveal(protectedValue: string): Promise<string> {
        try {
            const [version, encodedIv, encodedCiphertext, encodedTag, extra] = protectedValue.split('.');
            if (
                version !== 'v1' ||
                encodedIv === undefined ||
                encodedCiphertext === undefined ||
                encodedTag === undefined ||
                extra !== undefined
            ) {
                throw new Error('invalid envelope');
            }
            const iv = Buffer.from(encodedIv, 'base64url');
            const ciphertext = Buffer.from(encodedCiphertext, 'base64url');
            const tag = Buffer.from(encodedTag, 'base64url');
            if (iv.byteLength !== AES_IV_BYTES || tag.byteLength !== AES_TAG_BYTES) {
                throw new Error('invalid envelope');
            }
            const decipher = createDecipheriv('aes-256-gcm', this.encryptionKey, iv);
            decipher.setAuthTag(tag);
            return Buffer.concat([decipher.update(ciphertext), decipher.final()]).toString('utf8');
        } catch {
            throw new ImGatewayError('invalid_contract', 'Protected external identity is invalid');
        }
    }
}

/** 使用随机六位码和 HMAC 摘要实现短期配对。 */
export class HmacPairingCodePort implements PairingCodePort {
    private readonly key: Buffer;

    /** @param secret 由 Secret 注入的配对码摘要主密钥。 */
    public constructor(secret: string) {
        assertMinimumSecret(secret, 'IDENTITY_SECRET');
        this.key = deriveKey(secret, 'pairing-code');
    }

    /** {@inheritDoc PairingCodePort.issue} */
    public async issue(): Promise<{ readonly displayCode: string; readonly hash: string }> {
        const displayCode = String(randomInt(0, 1_000_000)).padStart(6, '0');
        return { displayCode, hash: await this.hash(displayCode) };
    }

    /** {@inheritDoc PairingCodePort.hash} */
    public hash(displayCode: string): Promise<string> {
        return Promise.resolve(createHmac('sha256', this.key).update(displayCode, 'utf8').digest('base64url'));
    }
}

/** 使用 UUID v4 生成生产聚合标识，并固定部署管理的渠道账号标识。 */
export class UuidIdGenerator implements IdGenerator {
    /** @param channelAccountId 当前部署管理的渠道账号标识。 */
    public constructor(private readonly channelAccountId: ChannelAccountId) {}

    /** {@inheritDoc IdGenerator.nextChannelAccountId} */
    public nextChannelAccountId(): ChannelAccountId {
        return this.channelAccountId;
    }

    /** {@inheritDoc IdGenerator.nextPairingSessionId} */
    public nextPairingSessionId(): PairingSessionId {
        return this.next('pairing');
    }

    /** {@inheritDoc IdGenerator.nextBindingId} */
    public nextBindingId(): BindingId {
        return this.next('binding');
    }

    /** {@inheritDoc IdGenerator.nextExternalIdentityId} */
    public nextExternalIdentityId(): ExternalIdentityId {
        return this.next('identity');
    }

    /** {@inheritDoc IdGenerator.nextDeliveryId} */
    public nextDeliveryId(): DeliveryId {
        return this.next('delivery');
    }

    /** {@inheritDoc IdGenerator.nextDeliveryAttemptId} */
    public nextDeliveryAttemptId(): DeliveryAttemptId {
        return this.next('attempt');
    }

    /** {@inheritDoc IdGenerator.nextDeliveryReceiptId} */
    public nextDeliveryReceiptId(): DeliveryReceiptId {
        return this.next('receipt');
    }

    /** {@inheritDoc IdGenerator.actionIdForDelivery} */
    public actionIdForDelivery(deliveryId: DeliveryId): ActionId {
        return unsafeId<ActionId>(`action-${deliveryId}`);
    }

    /** {@inheritDoc IdGenerator.nextOperationId} */
    public nextOperationId(): OperationId {
        return this.next('operation');
    }

    /** {@inheritDoc IdGenerator.nextOutboxEventId} */
    public nextOutboxEventId(): OutboxEventId {
        return this.next('outbox');
    }

    /** {@inheritDoc IdGenerator.nextRequestId} */
    public nextRequestId(): RequestId {
        return this.next('request');
    }

    private next<T>(prefix: string): T {
        return unsafeId<T>(`${prefix}-${randomUUID()}`);
    }
}

function assertMinimumSecret(value: string, name: string, minimum = MINIMUM_SECRET_BYTES): void {
    if (Buffer.byteLength(value, 'utf8') < minimum) {
        throw new Error(`${name} must contain at least ${String(minimum)} bytes`);
    }
}

function deriveKey(secret: string, purpose: string): Buffer {
    return createHmac('sha256', secret).update(`voicelife:${purpose}`, 'utf8').digest();
}

function digest(value: string): Buffer {
    return createHash('sha256').update(value, 'utf8').digest();
}

function unauthorized(): ImGatewayError {
    return new ImGatewayError('unauthorized', 'Device authorization is invalid');
}
