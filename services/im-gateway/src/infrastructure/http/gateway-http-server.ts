import { randomUUID } from 'node:crypto';
import { createServer, type IncomingMessage, type Server, type ServerResponse } from 'node:http';

import type { ImGatewayRuntime } from '../../app/create-im-gateway.js';
import type { ActionId, DeviceId, PairingSessionId } from '../../contracts/ids.js';
import { unsafeId } from '../../contracts/ids.js';
import type { ActionUiPageResponse } from './action-ui-api.js';
import { streamReminderActions } from './gateway-sse-response.js';
import { ImGatewayError } from '../../shared/errors.js';

const JSON_BODY_LIMIT = 64 * 1024;
const WECHAT_BODY_LIMIT = 64 * 1024;
const FORM_BODY_LIMIT = 8 * 1024;
const PAIRING_SESSION_PATH = /^\/v1\/im\/pairing-sessions\/([^/]+)$/u;
const ACTION_RESULT_PATH = /^\/v1\/devices\/([^/]+)\/reminder-actions\/([^/]+)\/result$/u;
const ACTION_STREAM_PATH = /^\/v1\/devices\/([^/]+)\/reminder-actions\/stream$/u;
const ACTION_UI_PATH = /^\/voicelife\/reminder-actions\/([^/]+)$/u;
const SCHEDULE_QUERY_PAGE_PATH = /^\/voicelife\/reminder-actions\/query-result\/([^/]+)$/u;

/** 结构化日志条目允许的 JSON 标量和字段集合。 */
export interface GatewayLogEntry {
    readonly level: 'info' | 'warn' | 'error';
    readonly event: string;
    readonly requestId?: string;
    readonly correlationId?: string;
    readonly route?: string;
    readonly method?: string;
    readonly status?: number;
    readonly durationMs?: number;
    readonly deliveryId?: string;
    readonly actionId?: string;
    readonly errorCode?: string;
}

/** 接收已脱敏结构化事件的日志端口。 */
export interface GatewayLogger {
    /**
     * 写出一个不含凭据、请求体和动态 URL 的结构化事件。
     * @param entry 已脱敏事件。
     */
    log(entry: GatewayLogEntry): void;
}

/** 生产 HTTP 监听器的装配参数。 */
export interface GatewayHttpServerOptions {
    readonly host: string;
    readonly port: number;
    readonly runtime: ImGatewayRuntime;
    readonly logger: GatewayLogger;
    /** 通知生产 Outbox worker 已有新 Delivery 可领取。 */
    readonly deliveryAvailable?: () => void;
    /** 可选的 SSE 心跳间隔，供基础设施层确定性验证使用。 */
    readonly sseHeartbeatIntervalMs?: number;
    /**
     * 探测数据库等关键依赖是否仍可用。
     * @returns 健康响应；抛错时监听器返回 503。
     */
    healthCheck(): Promise<{ readonly status: 'ok' }>;
}

/** 已启动的生产 HTTP 监听器。 */
export interface StartedGatewayHttpServer {
    readonly origin: string;
    /** @returns 监听器完全关闭后兑现的 Promise。 */
    close(): Promise<void>;
}

/**
 * 启动承载设备 API、SSE、Action UI 与微信 webhook 的生产监听器。
 * @param options 监听地址、运行时、健康检查和日志端口。
 * @returns 已开始监听的服务句柄。
 */
export async function startGatewayHttpServer(options: GatewayHttpServerOptions): Promise<StartedGatewayHttpServer> {
    validateAddress(options.host, options.port);
    const server = createServer((request, response) => {
        void handleRequest(request, response, options).catch((error: unknown) => {
            writeUnhandledError(response, error);
        });
    });
    await listen(server, options.host, options.port);
    const address = server.address();
    if (address === null || typeof address === 'string') {
        await closeServer(server);
        throw new Error('Gateway HTTP server did not expose a TCP address');
    }
    const originHost = address.family === 'IPv6' ? `[${address.address}]` : address.address;
    return {
        origin: `http://${originHost}:${String(address.port)}`,
        close: () => closeServer(server),
    };
}

async function handleRequest(
    request: IncomingMessage,
    response: ServerResponse,
    options: GatewayHttpServerOptions,
): Promise<void> {
    const requestId = randomUUID();
    const startedAt = performance.now();
    const url = new URL(request.url ?? '/', 'http://gateway.local');
    const method = request.method ?? 'GET';
    const context: RequestLogContext = { route: undefined, correlationId: undefined };
    response.once('finish', () => {
        safeLog(options.logger, {
            level: response.statusCode >= 500 ? 'error' : response.statusCode >= 400 ? 'warn' : 'info',
            event: 'http.request.completed',
            requestId,
            ...(context.route === undefined ? {} : { route: context.route }),
            ...(context.correlationId === undefined ? {} : { correlationId: context.correlationId }),
            method,
            status: response.statusCode,
            durationMs: Math.round(performance.now() - startedAt),
        });
    });

    try {
        await routeRequest(request, response, url, method, options, requestId, context);
    } catch (error) {
        safeLog(options.logger, {
            level: 'error',
            event: 'http.request.failed',
            requestId,
            ...(context.route === undefined ? {} : { route: context.route }),
            ...(context.correlationId === undefined ? {} : { correlationId: context.correlationId }),
            errorCode: safeErrorCode(error),
        });
        writeUnhandledError(response, error);
    }
}

async function routeRequest(
    request: IncomingMessage,
    response: ServerResponse,
    url: URL,
    method: string,
    options: GatewayHttpServerOptions,
    requestId: string,
    context: RequestLogContext,
): Promise<void> {
    if (url.pathname === '/healthz' && method === 'GET') {
        context.route = 'healthz';
        try {
            writeJson(response, 200, await options.healthCheck());
        } catch {
            writeJson(response, 503, { status: 'unavailable' });
        }
        return;
    }
    if (url.pathname === '/v1/im/pairing-sessions' && method === 'POST') {
        context.route = 'device.pairing.create';
        writeJson(
            response,
            201,
            await options.runtime.deviceApi.postPairingSession({
                authorization: authorization(request),
                body: await readJson(request),
            }),
        );
        return;
    }
    const pairingMatch = PAIRING_SESSION_PATH.exec(url.pathname);
    if (pairingMatch !== null && method === 'GET') {
        context.route = 'device.pairing.get';
        const session = await options.runtime.deviceApi.getPairingSession({
            authorization: authorization(request),
            pairingSessionId: unsafeId<PairingSessionId>(decodePathSegment(pairingMatch[1]!)),
        });
        if (session === undefined) writeText(response, 404, 'Not Found');
        else writeJson(response, 200, session);
        return;
    }
    if (url.pathname === '/v1/im/schedule-receipts' && method === 'POST') {
        context.route = 'device.schedule-receipt.create';
        const body = await readJson(request);
        const submission = await options.runtime.deviceApi.postScheduleReceipt({
            authorization: authorization(request),
            idempotencyKey: requiredHeader(request, 'idempotency-key'),
            body,
        });
        context.correlationId = correlationId(body);
        writeJson(response, 202, submission);
        notifyDeliveryWorker(submission.deliveries, options);
        return;
    }
    if (url.pathname === '/v1/im/schedule-query-results' && method === 'POST') {
        context.route = 'device.schedule-query-result.create';
        const body = await readJson(request);
        const submission = await options.runtime.deviceApi.postScheduleQueryResult({
            authorization: authorization(request),
            idempotencyKey: requiredHeader(request, 'idempotency-key'),
            body,
        });
        context.correlationId = correlationId(body);
        writeJson(response, 202, submission);
        notifyDeliveryWorker(submission.deliveries, options);
        return;
    }
    if (url.pathname === '/v1/im/notifications' && method === 'POST') {
        context.route = 'device.notification.create';
        const body = await readJson(request);
        const submission = await options.runtime.deviceApi.postNotification({
            authorization: authorization(request),
            idempotencyKey: requiredHeader(request, 'idempotency-key'),
            body,
        });
        context.correlationId = correlationId(body);
        writeJson(response, 202, submission);
        notifyDeliveryWorker(submission.deliveries, options);
        return;
    }
    if (url.pathname === '/v1/im/reminder-action-statuses' && method === 'POST') {
        context.route = 'device.voice-action-status.create';
        const body = await readJson(request);
        const actions = await options.runtime.deviceApi.postVoiceReminderActionStatus({
            authorization: authorization(request),
            idempotencyKey: requiredHeader(request, 'idempotency-key'),
            body,
        });
        context.correlationId = correlationId(body);
        writeJson(response, 200, { status: 'accepted', actions });
        return;
    }
    const actionResultMatch = ACTION_RESULT_PATH.exec(url.pathname);
    if (actionResultMatch !== null && method === 'POST') {
        context.route = 'device.action-result.create';
        const body = await readJson(request);
        const action = await options.runtime.deviceApi.postReminderActionResult({
            authorization: authorization(request),
            deviceId: unsafeId<DeviceId>(decodePathSegment(actionResultMatch[1]!)),
            commandId: unsafeId<ActionId>(decodePathSegment(actionResultMatch[2]!)),
            body,
        });
        context.correlationId = action.correlationId;
        writeJson(response, 200, action);
        return;
    }
    const actionStreamMatch = ACTION_STREAM_PATH.exec(url.pathname);
    if (actionStreamMatch !== null && method === 'GET') {
        context.route = 'device.action-stream.connect';
        await streamReminderActions({
            request,
            response,
            url,
            runtime: options.runtime,
            logger: options.logger,
            requestId,
            encodedDeviceId: actionStreamMatch[1]!,
            correlationIdObserved: (value) => {
                context.correlationId = value;
            },
            ...(options.sseHeartbeatIntervalMs === undefined
                ? {}
                : { heartbeatIntervalMs: options.sseHeartbeatIntervalMs }),
        });
        return;
    }
    if (url.pathname === '/wechat') {
        context.route = 'wechat.webhook';
        const webhookApi = options.runtime.wechatApi;
        if (webhookApi === undefined) throw new Error('WeChat webhook is not configured');
        if (method === 'GET') {
            writeText(response, 200, webhookApi.verify(webhookRequest(url)) ?? '');
            return;
        }
        if (method === 'POST') {
            writeWechatWebhookResponse(
                response,
                await webhookApi.post({ ...webhookRequest(url), body: await readBody(request, WECHAT_BODY_LIMIT) }),
            );
            return;
        }
        writeMethodNotAllowed(response, 'GET, POST');
        return;
    }
    const scheduleQueryPageMatch = SCHEDULE_QUERY_PAGE_PATH.exec(url.pathname);
    if (scheduleQueryPageMatch !== null) {
        context.route = 'schedule-query-page';
        if (method !== 'GET') {
            writeMethodNotAllowed(response, 'GET');
            return;
        }
        writePage(
            response,
            await options.runtime.scheduleQueryPageApi.get(decodePathSegment(scheduleQueryPageMatch[1]!)),
        );
        return;
    }
    const actionUiMatch = ACTION_UI_PATH.exec(url.pathname);
    if (actionUiMatch !== null) {
        context.route = 'action-ui';
        const token = decodePathSegment(actionUiMatch[1]!);
        if (method === 'GET') {
            writePage(response, await options.runtime.actionUiPageApi.get(token));
            return;
        }
        if (method === 'POST') {
            requireContentType(request, 'application/x-www-form-urlencoded');
            writePage(
                response,
                await options.runtime.actionUiPageApi.post(token, formInput(await readBody(request, FORM_BODY_LIMIT))),
            );
            return;
        }
        writeMethodNotAllowed(response, 'GET, POST');
        return;
    }
    writeText(response, 404, 'Not Found');
}

function notifyDeliveryWorker(
    deliveries: readonly { readonly deliveryId: string }[],
    options: GatewayHttpServerOptions,
): void {
    if (deliveries.length === 0) return;
    try {
        options.deliveryAvailable?.();
    } catch {
        // The polling worker remains the source of truth if an eager wake-up fails.
    }
}

function authorization(request: IncomingMessage): string {
    return request.headers.authorization ?? '';
}

function requiredHeader(request: IncomingMessage, name: string): string {
    const value = request.headers[name];
    if (typeof value !== 'string' || value.trim() === '') throw new InvalidRequestError();
    return value;
}

async function readJson(request: IncomingMessage): Promise<unknown> {
    requireContentType(request, 'application/json');
    const raw = await readBody(request, JSON_BODY_LIMIT);
    try {
        return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(raw));
    } catch {
        throw new InvalidRequestError();
    }
}

function requireContentType(request: IncomingMessage, expected: string): void {
    const actual = request.headers['content-type']?.split(';', 1)[0]?.trim().toLowerCase();
    if (actual !== expected) throw new UnsupportedMediaTypeError();
}

async function readBody(request: IncomingMessage, limit: number): Promise<Uint8Array> {
    const declaredLength = request.headers['content-length'];
    if (declaredLength !== undefined && Number(declaredLength) > limit) {
        request.resume();
        throw new RequestBodyTooLargeError();
    }
    const chunks: Buffer[] = [];
    let size = 0;
    let exceeded = false;
    for await (const rawChunk of request) {
        const chunk = Buffer.isBuffer(rawChunk) ? rawChunk : Buffer.from(rawChunk as Uint8Array);
        size += chunk.byteLength;
        if (size > limit) exceeded = true;
        else if (!exceeded) chunks.push(chunk);
    }
    if (exceeded) throw new RequestBodyTooLargeError();
    return Buffer.concat(chunks, size);
}

function formInput(body: Uint8Array): Record<string, string> {
    try {
        return Object.fromEntries(
            new URLSearchParams(new TextDecoder('utf-8', { fatal: true }).decode(body)).entries(),
        );
    } catch {
        throw new InvalidRequestError();
    }
}

function webhookRequest(url: URL): {
    readonly signature?: string;
    readonly timestamp?: string;
    readonly nonce?: string;
    readonly echostr?: string;
    readonly encrypt_type?: string;
} {
    const values = ['signature', 'timestamp', 'nonce', 'echostr', 'encrypt_type'] as const;
    return Object.fromEntries(
        values.flatMap((name) => {
            const value = url.searchParams.get(name);
            return value === null ? [] : [[name, value]];
        }),
    );
}

function correlationId(body: unknown): string | undefined {
    if (typeof body !== 'object' || body === null || Array.isArray(body)) return undefined;
    const value = (body as Record<string, unknown>).correlationId;
    return typeof value === 'string' && value.trim() !== '' ? value : undefined;
}

function decodePathSegment(value: string): string {
    try {
        return decodeURIComponent(value);
    } catch {
        throw new InvalidRequestError();
    }
}

function writePage(response: ServerResponse, page: ActionUiPageResponse): void {
    response.writeHead(page.status, page.headers);
    response.end(page.body);
}

function writeJson(response: ServerResponse, status: number, value: unknown): void {
    response.writeHead(status, responseHeaders('application/json; charset=utf-8'));
    response.end(serializeJson(value));
}

function serializeJson(value: unknown): string {
    const serialized = JSON.stringify(value);
    if (serialized === undefined) return 'null';
    return serialized
        .replaceAll('<', '\\u003c')
        .replaceAll('>', '\\u003e')
        .replaceAll('&', '\\u0026')
        .replaceAll('\u2028', '\\u2028')
        .replaceAll('\u2029', '\\u2029');
}

function writeText(response: ServerResponse, status: number, body: string): void {
    response.writeHead(status, responseHeaders('text/plain; charset=utf-8'));
    response.end(body);
}

function writeWechatWebhookResponse(
    response: ServerResponse,
    result: {
        readonly body: string;
        readonly contentType: 'application/xml; charset=utf-8' | 'text/plain; charset=utf-8';
    },
): void {
    response.writeHead(200, responseHeaders(result.contentType));
    response.end(result.body);
}

function responseHeaders(contentType: string): Record<string, string> {
    return {
        'content-type': contentType,
        'cache-control': 'no-store',
        'x-content-type-options': 'nosniff',
    };
}

function writeMethodNotAllowed(response: ServerResponse, allow: string): void {
    response.setHeader('allow', allow);
    writeText(response, 405, 'Method Not Allowed');
}

function writeUnhandledError(response: ServerResponse, error: unknown): void {
    if (response.headersSent) {
        response.destroy();
        return;
    }
    if (error instanceof RequestBodyTooLargeError) writeText(response, 413, 'Payload Too Large');
    else if (error instanceof UnsupportedMediaTypeError) writeText(response, 415, 'Unsupported Media Type');
    else if (error instanceof InvalidRequestError || error instanceof TypeError)
        writeText(response, 400, 'Bad Request');
    else if (error instanceof ImGatewayError) writeGatewayError(response, error);
    else writeText(response, 500, 'Internal Server Error');
}

function writeGatewayError(response: ServerResponse, error: ImGatewayError): void {
    switch (error.code) {
        case 'unauthorized':
            response.setHeader('www-authenticate', 'Bearer');
            writeJson(response, 401, { error: 'unauthorized' });
            return;
        case 'binding_not_found':
            writeJson(response, 404, { error: 'binding_not_found' });
            return;
        case 'delivery_not_found':
            writeJson(response, 404, { error: 'delivery_not_found' });
            return;
        case 'action_not_found':
            writeJson(response, 404, { error: 'action_not_found' });
            return;
        case 'action_expired':
            writeJson(response, 410, { error: 'action_expired' });
            return;
        case 'idempotency_conflict':
            writeJson(response, 409, { error: 'idempotency_conflict' });
            return;
        case 'duplicate_event':
            writeJson(response, 409, { error: 'duplicate_event' });
            return;
        case 'resource_exhausted':
            writeJson(response, 429, { error: 'resource_exhausted' });
            return;
        case 'invalid_transition':
            writeJson(response, 403, { error: 'invalid_transition' });
            return;
        case 'invalid_contract':
            writeJson(response, 400, { error: 'invalid_contract' });
            return;
        case 'pairing_code_invalid':
            writeJson(response, 400, { error: 'pairing_code_invalid' });
            return;
        case 'capability_not_supported':
            writeJson(response, 400, { error: 'capability_not_supported' });
            return;
        case 'not_implemented':
            writeJson(response, 400, { error: 'not_implemented' });
            return;
    }
}

function safeErrorCode(error: unknown): string {
    return error instanceof ImGatewayError ? error.code : error instanceof Error ? error.name : 'unknown_error';
}

function safeLog(logger: GatewayLogger, entry: GatewayLogEntry): void {
    try {
        logger.log(entry);
    } catch {
        // Logging failures must never change HTTP or delivery behavior.
    }
}

function validateAddress(host: string, port: number): void {
    if (host.trim() === '') throw new Error('Gateway host is invalid');
    if (!Number.isSafeInteger(port) || port < 0 || port > 65_535) throw new Error('Gateway port is invalid');
}

function listen(server: Server, host: string, port: number): Promise<void> {
    return new Promise((resolve, reject) => {
        const onError = (error: Error): void => {
            server.off('listening', onListening);
            reject(error);
        };
        const onListening = (): void => {
            server.off('error', onError);
            resolve();
        };
        server.once('error', onError);
        server.once('listening', onListening);
        server.listen(port, host);
    });
}

function closeServer(server: Server): Promise<void> {
    if (!server.listening) return Promise.resolve();
    return new Promise((resolve, reject) => {
        server.close((error) => (error === undefined ? resolve() : reject(error)));
        server.closeAllConnections();
    });
}

class RequestBodyTooLargeError extends Error {}
class UnsupportedMediaTypeError extends Error {}
class InvalidRequestError extends Error {}

interface RequestLogContext {
    route: string | undefined;
    correlationId: string | undefined;
}
