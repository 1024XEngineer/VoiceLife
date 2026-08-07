import { createHash, timingSafeEqual } from 'node:crypto';
import { createServer, type IncomingMessage, type Server, type ServerResponse } from 'node:http';

import type { ActionUiPageController, ActionUiPageResponse } from './action-ui-api.js';
import type { WechatWebhookController } from './wechat-api.js';
import { ImGatewayError } from '../../shared/errors.js';

const WECHAT_BODY_LIMIT = 64 * 1024;
const FORM_BODY_LIMIT = 8 * 1024;
const ACTION_UI_PATH = /^\/voicelife\/reminder-actions\/([^/]+)$/u;
const DELIVERY_PATH = /^\/__dev\/wechat\/deliveries\/([^/]+)$/u;

/** 开发联调端点返回的脱敏投递摘要。 */
export interface WechatDevDeliverySnapshot {
    readonly deliveryId: string;
    readonly status: string;
    readonly externalMessageId?: string;
    readonly attempts?: number;
    readonly receipts?: number;
}

/** 微信开发 HTTP harness 所需的控制器和受保护操作。 */
export interface WechatDevHttpHarnessOptions {
    readonly host?: string;
    readonly port?: number;
    readonly deviceToken: string;
    readonly webhookApi: Pick<WechatWebhookController, 'verify' | 'post'>;
    readonly actionUiPageApi: Pick<ActionUiPageController, 'get' | 'post'>;
    readonly sendTestNotification: () => Promise<WechatDevDeliverySnapshot>;
    readonly inspectDelivery: (deliveryId: string) => Promise<WechatDevDeliverySnapshot | undefined>;
}

/** 已启动的微信开发 HTTP harness。 */
export interface StartedWechatDevHttpHarness {
    readonly origin: string;
    /** @returns 监听器完全关闭后兑现的 Promise。 */
    close(): Promise<void>;
}

/**
 * 启动仅供真实微信联调使用的本地 HTTP harness。
 * @param options 控制器、鉴权令牌和监听地址。
 * @returns 已启动监听器及其本地 origin。
 */
export async function startWechatDevHttpHarness(
    options: WechatDevHttpHarnessOptions,
): Promise<StartedWechatDevHttpHarness> {
    const host = options.host ?? '127.0.0.1';
    const port = options.port ?? 3000;
    const deviceToken = options.deviceToken.trim();
    if (host !== '127.0.0.1' && host !== '::1') {
        throw new Error('WeChat development harness must bind to a loopback address');
    }
    if (!Number.isSafeInteger(port) || port < 0 || port > 65_535) {
        throw new Error('WeChat development harness port is invalid');
    }
    if (Buffer.byteLength(deviceToken, 'utf8') < 24) {
        throw new Error('WeChat development harness device token must contain at least 24 bytes');
    }

    const server = createServer((request, response) => {
        void routeRequest(request, response, options, deviceToken).catch((error: unknown) => {
            writeUnhandledError(response, error);
        });
    });
    await listen(server, host, port);
    const address = server.address();
    if (address === null || typeof address === 'string') {
        await closeServer(server);
        throw new Error('WeChat development harness did not expose a TCP address');
    }
    const originHost = address.family === 'IPv6' ? `[${address.address}]` : address.address;
    return {
        origin: `http://${originHost}:${String(address.port)}`,
        close: () => closeServer(server),
    };
}

async function routeRequest(
    request: IncomingMessage,
    response: ServerResponse,
    options: WechatDevHttpHarnessOptions,
    deviceToken: string,
): Promise<void> {
    const url = new URL(request.url ?? '/', 'http://localhost');
    const method = request.method ?? 'GET';

    if (url.pathname === '/healthz' && method === 'GET') {
        writeJson(response, 200, { status: 'ok' });
        return;
    }
    if (url.pathname === '/wechat') {
        if (method === 'GET') {
            const echo = options.webhookApi.verify(webhookRequest(url));
            writeText(response, 200, echo ?? '');
            return;
        }
        if (method === 'POST') {
            const body = await readBody(request, WECHAT_BODY_LIMIT);
            const result = await options.webhookApi.post({ ...webhookRequest(url), body });
            writeText(response, 200, result);
            return;
        }
        writeMethodNotAllowed(response, 'GET, POST');
        return;
    }

    const actionMatch = ACTION_UI_PATH.exec(url.pathname);
    if (actionMatch !== null) {
        const token = decodePathSegment(actionMatch[1]!);
        if (method === 'GET') {
            writePage(response, await options.actionUiPageApi.get(token));
            return;
        }
        if (method === 'POST') {
            const contentType = request.headers['content-type']?.split(';', 1)[0]?.trim().toLowerCase();
            if (contentType !== 'application/x-www-form-urlencoded') {
                writeText(response, 415, 'Unsupported Media Type');
                return;
            }
            const body = await readBody(request, FORM_BODY_LIMIT);
            writePage(response, await options.actionUiPageApi.post(token, formInput(body)));
            return;
        }
        writeMethodNotAllowed(response, 'GET, POST');
        return;
    }

    if (url.pathname === '/__dev/wechat/send-test' && method === 'POST') {
        if (!isAuthorized(request, deviceToken)) {
            writeUnauthorized(response);
            return;
        }
        writeJson(response, 200, await options.sendTestNotification());
        return;
    }

    const deliveryMatch = DELIVERY_PATH.exec(url.pathname);
    if (deliveryMatch !== null && method === 'GET') {
        if (!isAuthorized(request, deviceToken)) {
            writeUnauthorized(response);
            return;
        }
        const deliveryId = decodePathSegment(deliveryMatch[1]!);
        const snapshot = await options.inspectDelivery(deliveryId);
        if (snapshot === undefined) {
            writeText(response, 404, 'Not Found');
            return;
        }
        writeJson(response, 200, snapshot);
        return;
    }

    writeText(response, 404, 'Not Found');
}

function webhookRequest(url: URL): {
    readonly signature?: string;
    readonly timestamp?: string;
    readonly nonce?: string;
    readonly echostr?: string;
    readonly encrypt_type?: string;
} {
    const signature = url.searchParams.get('signature');
    const timestamp = url.searchParams.get('timestamp');
    const nonce = url.searchParams.get('nonce');
    const echostr = url.searchParams.get('echostr');
    const encryptType = url.searchParams.get('encrypt_type');
    return {
        ...(signature === null ? {} : { signature }),
        ...(timestamp === null ? {} : { timestamp }),
        ...(nonce === null ? {} : { nonce }),
        ...(echostr === null ? {} : { echostr }),
        ...(encryptType === null ? {} : { encrypt_type: encryptType }),
    };
}

function formInput(body: Uint8Array): Record<string, string> {
    const values = new URLSearchParams(new TextDecoder('utf-8', { fatal: true }).decode(body));
    return Object.fromEntries(values.entries());
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
        if (size > limit) {
            exceeded = true;
        } else if (!exceeded) {
            chunks.push(chunk);
        }
    }
    if (exceeded) throw new RequestBodyTooLargeError();
    return Buffer.concat(chunks, size);
}

function isAuthorized(request: IncomingMessage, expectedToken: string): boolean {
    const authorization = request.headers.authorization;
    if (authorization === undefined || !authorization.startsWith('Bearer ')) return false;
    const actual = createHash('sha256').update(authorization.slice(7), 'utf8').digest();
    const expected = createHash('sha256').update(expectedToken, 'utf8').digest();
    return timingSafeEqual(actual, expected);
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
    response.writeHead(status, {
        'content-type': 'application/json; charset=utf-8',
        'cache-control': 'no-store',
        'x-content-type-options': 'nosniff',
    });
    response.end(JSON.stringify(value));
}

function writeText(response: ServerResponse, status: number, body: string): void {
    response.writeHead(status, {
        'content-type': 'text/plain; charset=utf-8',
        'cache-control': 'no-store',
        'x-content-type-options': 'nosniff',
    });
    response.end(body);
}

function writeUnauthorized(response: ServerResponse): void {
    response.setHeader('www-authenticate', 'Bearer');
    writeText(response, 401, 'Unauthorized');
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
    if (error instanceof RequestBodyTooLargeError) {
        writeText(response, 413, 'Payload Too Large');
        return;
    }
    if (error instanceof InvalidRequestError || error instanceof ImGatewayError || error instanceof TypeError) {
        writeText(response, 400, 'Bad Request');
        return;
    }
    writeText(response, 500, 'Internal Server Error');
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
    });
}

class RequestBodyTooLargeError extends Error {}

class InvalidRequestError extends Error {}
