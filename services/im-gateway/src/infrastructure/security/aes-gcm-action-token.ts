import { createCipheriv, createDecipheriv, createHash, createHmac, randomBytes } from 'node:crypto';

import type { ActionTokenClaims, ActionTokenPort } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';

const TOKEN_VERSION = 'v1';
const TOKEN_AAD = Buffer.from('voicelife:action-token:v1', 'utf8');
const NONCE_BYTES = 12;
const TAG_BYTES = 16;
const MAX_TOKEN_BYTES = 4096;

/** 使用 AES-256-GCM 签发不可读、可跨进程校验的动作令牌。 */
export class AesGcmActionTokenPort implements ActionTokenPort {
    private readonly encryptionKey: Buffer;

    private readonly fingerprintKey: Buffer;

    /**
     * @param secret 由 Secret 引用解析后注入的部署密钥，至少 32 字节。
     * @param nonceSource 生成 12 字节随机 nonce 的来源，仅测试时替换。
     */
    public constructor(
        secret: string,
        private readonly nonceSource: (size: number) => Uint8Array = randomBytes,
    ) {
        if (Buffer.byteLength(secret, 'utf8') < 32 || Buffer.byteLength(secret, 'utf8') > 1024) {
            throw new ImGatewayError('invalid_contract', 'Action token secret must contain 32 to 1024 bytes');
        }
        this.encryptionKey = deriveKey(secret, 'encryption');
        this.fingerprintKey = deriveKey(secret, 'fingerprint');
    }

    /** {@inheritDoc ActionTokenPort.issue} */
    public async issue(claims: ActionTokenClaims): Promise<string> {
        validateClaims(claims);
        const nonce = Buffer.from(this.nonceSource(NONCE_BYTES));
        if (nonce.byteLength !== NONCE_BYTES) {
            throw new ImGatewayError('invalid_contract', 'Action token nonce source returned an invalid nonce');
        }
        const cipher = createCipheriv('aes-256-gcm', this.encryptionKey, nonce);
        cipher.setAAD(TOKEN_AAD);
        const plaintext = Buffer.from(JSON.stringify(claims), 'utf8');
        const encrypted = Buffer.concat([cipher.update(plaintext), cipher.final()]);
        const tag = cipher.getAuthTag();
        return [
            TOKEN_VERSION,
            nonce.toString('base64url'),
            encrypted.toString('base64url'),
            tag.toString('base64url'),
        ].join('.');
    }

    /** {@inheritDoc ActionTokenPort.verify} */
    public verify(token: string): Promise<ActionTokenClaims> {
        try {
            if (Buffer.byteLength(token, 'utf8') > MAX_TOKEN_BYTES) throw new Error('oversized token');
            const pieces = token.split('.');
            if (pieces.length !== 4 || pieces[0] !== TOKEN_VERSION) throw new Error('invalid token envelope');
            const nonce = decodeBase64Url(pieces[1]!, NONCE_BYTES);
            const encrypted = decodeBase64Url(pieces[2]!);
            const tag = decodeBase64Url(pieces[3]!, TAG_BYTES);
            const decipher = createDecipheriv('aes-256-gcm', this.encryptionKey, nonce);
            decipher.setAAD(TOKEN_AAD);
            decipher.setAuthTag(tag);
            const plaintext = Buffer.concat([decipher.update(encrypted), decipher.final()]).toString('utf8');
            const claims = parseClaims(JSON.parse(plaintext) as unknown);
            return Promise.resolve(claims);
        } catch {
            return Promise.reject(new ImGatewayError('action_not_found', 'Action token is invalid'));
        }
    }

    /** {@inheritDoc ActionTokenPort.fingerprint} */
    public fingerprint(token: string): Promise<string> {
        if (token === '' || Buffer.byteLength(token, 'utf8') > MAX_TOKEN_BYTES) {
            return Promise.reject(new ImGatewayError('action_not_found', 'Action token is invalid'));
        }
        return Promise.resolve(createHmac('sha256', this.fingerprintKey).update(token, 'utf8').digest('base64url'));
    }
}

function deriveKey(secret: string, purpose: string): Buffer {
    return createHash('sha256').update(`voicelife:action-token:${purpose}:`, 'utf8').update(secret, 'utf8').digest();
}

function decodeBase64Url(value: string, expectedLength?: number): Buffer {
    if (value === '' || !/^[A-Za-z0-9_-]+$/u.test(value)) throw new Error('invalid base64url');
    const decoded = Buffer.from(value, 'base64url');
    if (
        decoded.toString('base64url') !== value ||
        (expectedLength !== undefined && decoded.byteLength !== expectedLength)
    ) {
        throw new Error('invalid base64url length');
    }
    return decoded;
}

function validateClaims(claims: ActionTokenClaims): void {
    if (
        claims.actionId.trim() === '' ||
        claims.actionId.length > 256 ||
        claims.deliveryId.trim() === '' ||
        claims.deliveryId.length > 256 ||
        !isCanonicalIsoDateTime(claims.expiresAt)
    ) {
        throw new ImGatewayError('invalid_contract', 'Action token claims are invalid');
    }
}

function parseClaims(value: unknown): ActionTokenClaims {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) throw new Error('invalid claims');
    const record = value as Record<string, unknown>;
    if (
        Object.keys(record).length !== 3 ||
        typeof record.actionId !== 'string' ||
        typeof record.deliveryId !== 'string' ||
        typeof record.expiresAt !== 'string'
    ) {
        throw new Error('invalid claims');
    }
    const claims = record as unknown as ActionTokenClaims;
    validateClaims(claims);
    return claims;
}

function isCanonicalIsoDateTime(value: string): boolean {
    const milliseconds = Date.parse(value);
    return Number.isFinite(milliseconds) && new Date(milliseconds).toISOString() === value;
}
