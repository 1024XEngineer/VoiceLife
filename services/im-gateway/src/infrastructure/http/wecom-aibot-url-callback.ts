import { createDecipheriv, createHash, timingSafeEqual } from 'node:crypto';

import type { NormalizedImEvent } from '../../contracts/platform-events.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { WecomAibotInboundAdapter } from '../wecom/wecom-aibot-inbound-adapter.js';

/** 企业微信 AI Bot URL 回调的 query 参数与原始请求体。 */
export interface WecomAibotUrlCallbackRequest {
    readonly timestamp?: string;
    readonly nonce?: string;
    readonly msg_signature?: string;
    readonly echostr?: string;
    readonly body?: string | Uint8Array;
}

/** 企业微信 AI Bot URL 回调成功响应。 */
export interface WecomAibotUrlCallbackResponse {
    readonly status: 200;
    readonly body: 'success';
}

/** 企业微信 AI Bot URL 回调控制器的部署级配置。 */
export interface WecomAibotUrlCallbackOptions {
    readonly token: string;
    readonly encodingAesKey: string;
    readonly postEvent: (event: NormalizedImEvent) => Promise<void>;
}

/**
 * 处理企业微信 AI Bot 的加密 URL 回调。
 *
 * 控制器只接受通过 Token 签名验证且能用 EncodingAESKey 解密的请求，原始密文和消息正文不会被记录。
 */
export class WecomAibotUrlCallbackController {
    private readonly token: string;

    private readonly aesKey: Buffer;

    /**
     * @param adapter 企业微信消息归一化适配器。
     * @param options 企业微信后台配置与事件提交入口。
     */
    public constructor(
        private readonly adapter: WecomAibotInboundAdapter,
        private readonly options: WecomAibotUrlCallbackOptions,
    ) {
        this.token = requiredOption(options.token, 'token');
        this.aesKey = decodeEncodingAesKey(options.encodingAesKey);
    }

    /**
     * 验证企业微信后台配置请求并返回解密后的 echostr。
     * @param request 已由 HTTP 框架映射的企业微信 query 参数。
     * @returns 不带引号或换行的明文 echostr。
     */
    public verify(request: WecomAibotUrlCallbackRequest): string {
        const echostr = requiredString(request.echostr, 'echostr');
        this.verifySignature(request, echostr);
        return this.decrypt(echostr);
    }

    /**
     * 解密并提交企业微信消息回调。
     * @param request 已由 HTTP 框架映射的企业微信 query 参数与请求体。
     * @returns 企业微信要求的成功文本。
     */
    public async post(request: WecomAibotUrlCallbackRequest): Promise<WecomAibotUrlCallbackResponse> {
        const body = requiredStringBody(request.body);
        const encrypted = encryptedBody(body);
        this.verifySignature(request, encrypted);
        const rawEvent = parseJson(this.decrypt(encrypted));
        await this.options.postEvent(await this.adapter.normalizeInbound(rawEvent));
        return { status: 200, body: 'success' };
    }

    private verifySignature(request: WecomAibotUrlCallbackRequest, encrypted: string): void {
        const timestamp = requiredString(request.timestamp, 'timestamp');
        const nonce = requiredString(request.nonce, 'nonce');
        const signature = requiredString(request.msg_signature, 'msg_signature');
        const expected = createHash('sha1')
            .update([this.token, timestamp, nonce, encrypted].sort().join(''))
            .digest('hex');
        if (!safeEqual(signature, expected)) {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback signature is invalid');
        }
    }

    private decrypt(encrypted: string): string {
        const ciphertext = decodeCiphertext(encrypted);
        let plaintext: Buffer;
        try {
            const decipher = createDecipheriv('aes-256-cbc', this.aesKey, this.aesKey.subarray(0, 16));
            plaintext = Buffer.concat([decipher.update(ciphertext), decipher.final()]);
        } catch {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback cannot be decrypted');
        }
        if (plaintext.length < 20) {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback plaintext is invalid');
        }
        const messageLength = plaintext.readUInt32BE(16);
        const messageEnd = 20 + messageLength;
        if (!Number.isSafeInteger(messageEnd) || messageEnd > plaintext.length) {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback plaintext is invalid');
        }
        if (plaintext.subarray(messageEnd).length !== 0) {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback targets another receive ID');
        }
        try {
            return new TextDecoder('utf-8', { fatal: true }).decode(plaintext.subarray(20, messageEnd));
        } catch {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback message is not UTF-8');
        }
    }
}

function requiredOption(value: string, name: string): string {
    const normalized = value.trim();
    if (normalized === '') throw new ImGatewayError('invalid_contract', `WeCom AI Bot URL callback requires a ${name}`);
    return normalized;
}

function decodeEncodingAesKey(value: string): Buffer {
    const normalized = value.trim();
    if (!/^[A-Za-z0-9+/]{43}$/u.test(normalized)) {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot EncodingAESKey must be 43 base64 characters');
    }
    const key = Buffer.from(`${normalized}=`, 'base64');
    if (key.length !== 32) {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot EncodingAESKey must decode to 32 bytes');
    }
    return key;
}

function requiredString(value: string | undefined, name: string): string {
    if (value === undefined || value.trim() === '') {
        throw new ImGatewayError('invalid_contract', `WeCom AI Bot callback ${name} is required`);
    }
    return value;
}

function requiredStringBody(value: string | Uint8Array | undefined): string {
    if (typeof value === 'string') return value;
    if (value === undefined) throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback body is required');
    try {
        return new TextDecoder('utf-8', { fatal: true }).decode(value);
    } catch {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback body is not UTF-8');
    }
}

function encryptedBody(body: string): string {
    const parsed = parseJson(body);
    if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed) || typeof parsed.encrypt !== 'string') {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback body must contain encrypt');
    }
    return parsed.encrypt;
}

function parseJson(value: string): Record<string, unknown> {
    try {
        const parsed: unknown = JSON.parse(value);
        if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
            throw new Error('not an object');
        }
        return parsed as Record<string, unknown>;
    } catch {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback JSON is invalid');
    }
}

function decodeCiphertext(value: string): Buffer {
    if (!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/u.test(value)) {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback ciphertext is invalid');
    }
    const ciphertext = Buffer.from(value, 'base64');
    if (ciphertext.length === 0 || ciphertext.length % 16 !== 0) {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot callback ciphertext is invalid');
    }
    return ciphertext;
}

function safeEqual(actual: string, expected: string): boolean {
    const actualBytes = Buffer.from(actual, 'utf8');
    const expectedBytes = Buffer.from(expected, 'utf8');
    return actualBytes.length === expectedBytes.length && timingSafeEqual(actualBytes, expectedBytes);
}
