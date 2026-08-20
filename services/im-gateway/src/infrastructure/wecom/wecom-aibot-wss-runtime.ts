import WebSocket from 'ws';

import type { NormalizedImEvent } from '../../contracts/platform-events.js';
import type { ImSendAcceptance } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { WecomAibotInboundAdapter } from './wecom-aibot-inbound-adapter.js';

const DEFAULT_ENDPOINT = 'wss://openws.work.weixin.qq.com';
const HEARTBEAT_MILLISECONDS = 30_000;
const INITIAL_RECONNECT_MILLISECONDS = 1_000;
const MAX_RECONNECT_MILLISECONDS = 30_000;
const CLOSE_GRACE_MILLISECONDS = 5_000;
const SUBSCRIPTION_ACK_TIMEOUT_MILLISECONDS = 10_000;
const OUTBOUND_ACK_TIMEOUT_MILLISECONDS = 10_000;
const MAX_CHAT_ID_BYTES = 512;
const MAX_MARKDOWN_BYTES = 20_480;
const RETRYABLE_SEND_ERROR_CODES = new Set([-1, 408, 429, 45009, 45011, 502, 503, 504]);

/** 企业微信 AI Bot WSS 连接所需的最小 WebSocket 接口。 */
export interface WecomAibotWebSocket {
    /**
     * 注册 WebSocket 生命周期监听器。
     * @param type 需要监听的连接生命周期事件。
     * @param listener 对应事件的处理函数。
     */
    addEventListener(
        type: 'open' | 'message' | 'close' | 'error',
        listener: (event: { readonly data?: unknown }) => void,
    ): void;
    /**
     * 发送 UTF-8 JSON 文本帧。
     * @param data 要发送的 JSON 文本。
     */
    send(data: string): void;
    /** 主动关闭连接。 */
    close(): void;
    /** 立即终止无法完成关闭握手的连接。 */
    terminate(): void;
}

/** 创建企业微信 AI Bot WebSocket 客户端的可替换工厂。 */
export type WecomAibotWebSocketFactory = (endpoint: string) => WecomAibotWebSocket;

/** 企业微信 AI Bot WSS 运行时配置；Secret 仅保留在内存中。 */
export interface WecomAibotWssRuntimeOptions {
    readonly adapter: WecomAibotInboundAdapter;
    readonly botId: string;
    readonly secret: string;
    readonly postEvent: (event: NormalizedImEvent) => Promise<void>;
    readonly endpoint?: string;
    readonly createWebSocket?: WecomAibotWebSocketFactory;
    readonly nextRequestId?: () => string;
    /** 仅用于受控测试的心跳间隔；生产默认 30 秒。 */
    readonly heartbeatMilliseconds?: number;
    /** 仅用于受控测试的首次重连延迟；生产默认 1 秒。 */
    readonly reconnectDelayMilliseconds?: number;
    /** 仅用于受控测试的关闭等待时间；生产默认 5 秒。 */
    readonly closeGraceMilliseconds?: number;
    /** 仅用于受控测试的订阅 ACK 等待时间；生产默认 10 秒。 */
    readonly subscriptionAckTimeoutMilliseconds?: number;
    /** 仅用于受控测试的出站 ACK 等待时间；生产默认 10 秒。 */
    readonly outboundAckTimeoutMilliseconds?: number;
}

/**
 * 维护企业微信 AI Bot 的单个 WSS 入站连接。
 *
 * 运行时只把已认证的单聊文本帧交给 Gateway Application；不记录机器人 Secret 或原始消息。
 */
export class WecomAibotWssRuntime {
    private readonly endpoint: string;

    private readonly createWebSocket: WecomAibotWebSocketFactory;

    private readonly nextRequestId: () => string;

    private readonly heartbeatMilliseconds: number;

    private readonly initialReconnectMilliseconds: number;

    private readonly closeGraceMilliseconds: number;

    private readonly subscriptionAckTimeoutMilliseconds: number;

    private readonly outboundAckTimeoutMilliseconds: number;

    private socket: WecomAibotWebSocket | undefined;

    private subscriptionRequestId: string | undefined;

    private subscriptionAckTimeout: ReturnType<typeof setTimeout> | undefined;

    private heartbeat: ReturnType<typeof setInterval> | undefined;

    private reconnect: ReturnType<typeof setTimeout> | undefined;

    private closed = false;

    private nextReconnectMilliseconds: number;

    private readonly outboundRequests = new Map<
        string,
        {
            readonly resolve: (acceptance: ImSendAcceptance) => void;
            readonly timeout: ReturnType<typeof setTimeout>;
        }
    >();

    /** 连接已订阅并可接收平台回调时为 true。 */
    public healthy = false;

    /** @param options 连接身份、入站适配器与 Application 入口。 */
    public constructor(private readonly options: WecomAibotWssRuntimeOptions) {
        this.endpoint = options.endpoint?.trim() || DEFAULT_ENDPOINT;
        if (options.botId.trim() === '' || options.secret.trim() === '') {
            throw new ImGatewayError('invalid_contract', 'WeCom AI Bot WSS requires a bot ID and secret');
        }
        this.createWebSocket = options.createWebSocket ?? defaultWebSocket;
        this.nextRequestId = options.nextRequestId ?? (() => globalThis.crypto.randomUUID());
        this.heartbeatMilliseconds = positiveMilliseconds(options.heartbeatMilliseconds, HEARTBEAT_MILLISECONDS);
        this.initialReconnectMilliseconds = positiveMilliseconds(
            options.reconnectDelayMilliseconds,
            INITIAL_RECONNECT_MILLISECONDS,
        );
        this.closeGraceMilliseconds = positiveMilliseconds(options.closeGraceMilliseconds, CLOSE_GRACE_MILLISECONDS);
        this.subscriptionAckTimeoutMilliseconds = positiveMilliseconds(
            options.subscriptionAckTimeoutMilliseconds,
            SUBSCRIPTION_ACK_TIMEOUT_MILLISECONDS,
        );
        this.outboundAckTimeoutMilliseconds = positiveMilliseconds(
            options.outboundAckTimeoutMilliseconds,
            OUTBOUND_ACK_TIMEOUT_MILLISECONDS,
        );
        this.nextReconnectMilliseconds = this.initialReconnectMilliseconds;
    }

    /** 建立连接；重复调用不会创建第二个活动连接。 */
    public start(): void {
        if (this.socket !== undefined || this.reconnect !== undefined || this.closed) return;
        const socket = this.createWebSocket(this.endpoint);
        this.socket = socket;
        socket.addEventListener('open', () => this.subscribe(socket));
        socket.addEventListener('message', (event) => void this.receive(socket, event.data));
        socket.addEventListener('close', () => this.disconnected(socket));
        socket.addEventListener('error', () => undefined);
    }

    /**
     * 停止心跳、取消重连并关闭当前连接；关闭后不可重启。
     * @returns 当前连接关闭后兑现。
     */
    public async close(): Promise<void> {
        this.closed = true;
        if (this.reconnect !== undefined) clearTimeout(this.reconnect);
        this.reconnect = undefined;
        const socket = this.socket;
        this.unavailable(socket);
        if (socket !== undefined) await this.closeSocket(socket);
    }

    /**
     * 通过当前已订阅的连接向单聊会话主动发送 Markdown。
     *
     * 平台确认的是受理结果，不代表用户已阅读或设备已执行后续动作。
     * @param chatId 企业微信单聊标识。
     * @param content UTF-8 Markdown 正文。
     * @returns 企业微信的即时受理或可重试/不可重试失败分类。
     */
    public sendMarkdown(chatId: string, content: string): Promise<ImSendAcceptance> {
        const normalizedChatId = chatId.trim();
        const normalizedContent = content.trim();
        if (
            normalizedChatId === '' ||
            Buffer.byteLength(normalizedChatId, 'utf8') > MAX_CHAT_ID_BYTES ||
            normalizedContent === '' ||
            Buffer.byteLength(normalizedContent, 'utf8') > MAX_MARKDOWN_BYTES
        ) {
            return Promise.resolve({ accepted: false, retryable: false, errorCode: 'wecom_aibot_invalid_message' });
        }
        const socket = this.socket;
        if (!this.healthy || socket === undefined || this.closed) {
            return Promise.resolve({ accepted: false, retryable: true, errorCode: 'wecom_aibot_unavailable' });
        }
        const requestId = this.nextRequestId();
        return new Promise((resolve) => {
            const timeout = setTimeout(() => {
                if (!this.outboundRequests.delete(requestId)) return;
                resolve({ accepted: false, retryable: true, errorCode: 'wecom_aibot_timeout' });
            }, this.outboundAckTimeoutMilliseconds);
            this.outboundRequests.set(requestId, { resolve, timeout });
            try {
                socket.send(
                    JSON.stringify({
                        cmd: 'aibot_send_msg',
                        headers: { req_id: requestId },
                        body: {
                            chatid: normalizedChatId,
                            msgtype: 'markdown',
                            markdown: { content: normalizedContent },
                        },
                    }),
                );
            } catch {
                this.settleOutbound(requestId, {
                    accepted: false,
                    retryable: true,
                    errorCode: 'wecom_aibot_transport_error',
                });
            }
        });
    }

    private subscribe(socket: WecomAibotWebSocket): void {
        if (socket !== this.socket || this.closed) return;
        const requestId = this.nextRequestId();
        this.subscriptionRequestId = requestId;
        socket.send(
            JSON.stringify({
                cmd: 'aibot_subscribe',
                headers: { req_id: requestId },
                body: { bot_id: this.options.botId, secret: this.options.secret },
            }),
        );
        this.subscriptionAckTimeout = setTimeout(() => {
            if (socket !== this.socket || this.closed || this.subscriptionRequestId !== requestId) return;
            socket.close();
        }, this.subscriptionAckTimeoutMilliseconds);
    }

    private async receive(socket: WecomAibotWebSocket, data: unknown): Promise<void> {
        if (socket !== this.socket || this.closed || typeof data !== 'string') return;
        let frame: Record<string, unknown>;
        try {
            const parsed: unknown = JSON.parse(data);
            if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) return;
            frame = parsed as Record<string, unknown>;
        } catch {
            return;
        }
        if (this.isSubscriptionResponse(frame)) {
            this.clearSubscriptionAckTimeout();
            this.subscriptionRequestId = undefined;
            if (frame.errcode === 0) {
                this.healthy = true;
                this.nextReconnectMilliseconds = this.initialReconnectMilliseconds;
                this.startHeartbeat(socket);
            } else {
                socket.close();
            }
            return;
        }
        if (this.settleOutboundResponse(frame)) return;
        if (!this.healthy || frame.cmd !== 'aibot_msg_callback') return;
        try {
            await this.options.postEvent(await this.options.adapter.normalizeInbound(frame.body));
        } catch {
            // Untrusted platform input must not terminate the connection loop.
        }
    }

    private isSubscriptionResponse(frame: Record<string, unknown>): boolean {
        const headers = frame.headers;
        return (
            headers !== null &&
            typeof headers === 'object' &&
            (headers as Record<string, unknown>).req_id === this.subscriptionRequestId &&
            typeof frame.errcode === 'number'
        );
    }

    private startHeartbeat(socket: WecomAibotWebSocket): void {
        if (this.heartbeat !== undefined) return;
        this.heartbeat = setInterval(() => {
            if (socket !== this.socket || this.closed) return;
            socket.send(JSON.stringify({ cmd: 'ping', headers: { req_id: this.nextRequestId() } }));
        }, this.heartbeatMilliseconds);
    }

    private disconnected(socket: WecomAibotWebSocket): void {
        if (socket !== this.socket) return;
        this.unavailable(socket);
        if (this.closed || this.reconnect !== undefined) return;
        const delay = this.nextReconnectMilliseconds;
        this.nextReconnectMilliseconds = Math.min(MAX_RECONNECT_MILLISECONDS, delay * 2);
        this.reconnect = setTimeout(() => {
            this.reconnect = undefined;
            this.start();
        }, delay);
    }

    private unavailable(socket: WecomAibotWebSocket | undefined): void {
        if (socket !== undefined && socket !== this.socket) return;
        this.healthy = false;
        this.clearSubscriptionAckTimeout();
        this.subscriptionRequestId = undefined;
        if (this.heartbeat !== undefined) clearInterval(this.heartbeat);
        this.heartbeat = undefined;
        this.socket = undefined;
        for (const requestId of this.outboundRequests.keys()) {
            this.settleOutbound(requestId, {
                accepted: false,
                retryable: true,
                errorCode: 'wecom_aibot_unavailable',
            });
        }
    }

    private settleOutboundResponse(frame: Record<string, unknown>): boolean {
        const headers = frame.headers;
        if (headers === null || typeof headers !== 'object') return false;
        const requestId = (headers as Record<string, unknown>).req_id;
        if (
            typeof requestId !== 'string' ||
            !this.outboundRequests.has(requestId) ||
            typeof frame.errcode !== 'number'
        ) {
            return false;
        }
        const messageId = platformMessageId(frame.body);
        this.settleOutbound(
            requestId,
            frame.errcode === 0
                ? { accepted: true, ...(messageId === undefined ? {} : { platformMessageId: messageId }) }
                : {
                      accepted: false,
                      retryable: RETRYABLE_SEND_ERROR_CODES.has(frame.errcode),
                      errorCode: `wecom_aibot_${String(frame.errcode)}`,
                  },
        );
        return true;
    }

    private settleOutbound(requestId: string, acceptance: ImSendAcceptance): void {
        const pending = this.outboundRequests.get(requestId);
        if (pending === undefined) return;
        this.outboundRequests.delete(requestId);
        clearTimeout(pending.timeout);
        pending.resolve(acceptance);
    }

    private clearSubscriptionAckTimeout(): void {
        if (this.subscriptionAckTimeout !== undefined) clearTimeout(this.subscriptionAckTimeout);
        this.subscriptionAckTimeout = undefined;
    }

    private closeSocket(socket: WecomAibotWebSocket): Promise<void> {
        return new Promise((resolve) => {
            let closed = false;
            const finish = (): void => {
                if (closed) return;
                closed = true;
                clearTimeout(timeout);
                resolve();
            };
            socket.addEventListener('close', finish);
            const timeout = setTimeout(() => {
                socket.terminate();
                finish();
            }, this.closeGraceMilliseconds);
            socket.close();
        });
    }
}

function positiveMilliseconds(value: number | undefined, fallback: number): number {
    return value === undefined || !Number.isSafeInteger(value) || value <= 0 ? fallback : value;
}

function platformMessageId(body: unknown): string | undefined {
    if (body === null || typeof body !== 'object' || Array.isArray(body)) return undefined;
    const value = (body as Record<string, unknown>).msgid;
    return typeof value === 'string' && value.trim() !== '' ? value : undefined;
}

function defaultWebSocket(endpoint: string): WecomAibotWebSocket {
    const socket = new WebSocket(endpoint);
    return {
        addEventListener(type, listener): void {
            socket.on(type, (...args: unknown[]) => {
                listener(type === 'message' ? { data: args[0]?.toString() } : {});
            });
        },
        send(data): void {
            socket.send(data);
        },
        close(): void {
            socket.close();
        },
        terminate(): void {
            socket.terminate();
        },
    };
}
