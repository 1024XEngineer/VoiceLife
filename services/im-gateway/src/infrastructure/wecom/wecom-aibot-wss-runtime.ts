import WebSocket from 'ws';

import type { NormalizedImEvent } from '../../contracts/platform-events.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { WecomAibotInboundAdapter } from './wecom-aibot-inbound-adapter.js';

const DEFAULT_ENDPOINT = 'wss://openws.work.weixin.qq.com';
const HEARTBEAT_MILLISECONDS = 30_000;
const INITIAL_RECONNECT_MILLISECONDS = 1_000;
const MAX_RECONNECT_MILLISECONDS = 30_000;

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

    private socket: WecomAibotWebSocket | undefined;

    private subscriptionRequestId: string | undefined;

    private heartbeat: ReturnType<typeof setInterval> | undefined;

    private reconnect: ReturnType<typeof setTimeout> | undefined;

    private closed = false;

    private nextReconnectMilliseconds: number;

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
        socket?.close();
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
            if (frame.errcode === 0) {
                this.healthy = true;
                this.nextReconnectMilliseconds = this.initialReconnectMilliseconds;
                this.startHeartbeat(socket);
            } else {
                socket.close();
            }
            return;
        }
        if (!this.healthy || frame.cmd !== 'aibot_msg_callback') return;
        try {
            await this.options.postEvent(await this.options.adapter.normalizeInbound(frame));
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
        this.subscriptionRequestId = undefined;
        if (this.heartbeat !== undefined) clearInterval(this.heartbeat);
        this.heartbeat = undefined;
        this.socket = undefined;
    }
}

function positiveMilliseconds(value: number | undefined, fallback: number): number {
    return value === undefined || !Number.isSafeInteger(value) || value <= 0 ? fallback : value;
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
    };
}
