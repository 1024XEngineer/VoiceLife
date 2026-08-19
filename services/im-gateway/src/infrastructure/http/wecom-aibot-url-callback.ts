import { createRequire } from 'node:module';
import type WXBizMsgCryptType from 'wxcrypt';

const WXBizMsgCrypt = createRequire(import.meta.url)('wxcrypt') as typeof WXBizMsgCryptType;

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
    private readonly crypt: {
        verifyURL(msgSignature: string, timestamp: string, nonce: string, echostr: string): string;
    };

    /**
     * @param adapter 企业微信消息归一化适配器。
     * @param options 企业微信后台配置与事件提交入口。
     */
    public constructor(
        private readonly adapter: WecomAibotInboundAdapter,
        private readonly options: WecomAibotUrlCallbackOptions,
    ) {
        const token = requiredOption(options.token, 'token');
        const encodingAesKey = requiredOption(options.encodingAesKey, 'encodingAesKey');
        validateEncodingAesKey(encodingAesKey);
        this.crypt = new WXBizMsgCrypt(token, encodingAesKey, '');
    }

    /**
     * 验证企业微信后台配置请求并返回解密后的 echostr。
     * @param request 已由 HTTP 框架映射的企业微信 query 参数。
     * @returns 不带引号或换行的明文 echostr。
     */
    public verify(request: WecomAibotUrlCallbackRequest): string {
        const echostr = requiredString(request.echostr, 'echostr');
        return this.decrypt(request, echostr);
    }

    /**
     * 解密并提交企业微信消息回调。
     * @param request 已由 HTTP 框架映射的企业微信 query 参数与请求体。
     * @returns 企业微信要求的成功文本。
     */
    public async post(request: WecomAibotUrlCallbackRequest): Promise<WecomAibotUrlCallbackResponse> {
        const body = requiredStringBody(request.body);
        const encrypted = encryptedBody(body);
        const rawEvent = parseJson(this.decrypt(request, encrypted));
        await this.options.postEvent(await this.adapter.normalizeInbound(rawEvent));
        return { status: 200, body: 'success' };
    }

    private decrypt(request: WecomAibotUrlCallbackRequest, encrypted: string): string {
        const timestamp = requiredString(request.timestamp, 'timestamp');
        const nonce = requiredString(request.nonce, 'nonce');
        const signature = requiredString(request.msg_signature, 'msg_signature');
        try {
            return this.crypt.verifyURL(signature, timestamp, nonce, encrypted);
        } catch {
            throw new ImGatewayError(
                'invalid_contract',
                'WeCom AI Bot callback signature is invalid or cannot be decrypted',
            );
        }
    }
}

function requiredOption(value: string, name: string): string {
    const normalized = value.trim();
    if (normalized === '') throw new ImGatewayError('invalid_contract', `WeCom AI Bot URL callback requires a ${name}`);
    return normalized;
}

function validateEncodingAesKey(value: string): void {
    if (!/^[A-Za-z0-9+/]{43}$/u.test(value)) {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot EncodingAESKey must be 43 base64 characters');
    }
    const key = Buffer.from(`${value}=`, 'base64');
    if (key.length !== 32) {
        throw new ImGatewayError('invalid_contract', 'WeCom AI Bot EncodingAESKey must decode to 32 bytes');
    }
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
