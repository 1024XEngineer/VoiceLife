import type { IncomingMessage, ServerResponse } from 'node:http';

import type { ImGatewayRuntime } from '../../app/create-im-gateway.js';
import { unsafeId, type ActionId, type DeviceId, type ReminderTriggerId } from '../../contracts/ids.js';
import { SSE_HEARTBEAT_INTERVAL_SECONDS, SSE_RESPONSE_HEADERS } from './device-api.js';
import type { GatewayLogger } from './gateway-http-server.js';

/** 将一条动作流连接序列化到 HTTP 响应所需的依赖。 */
export interface ReminderActionStreamHttpOptions {
    readonly request: IncomingMessage;
    readonly response: ServerResponse;
    readonly url: URL;
    readonly runtime: ImGatewayRuntime;
    readonly logger: GatewayLogger;
    readonly requestId: string;
    readonly encodedDeviceId: string;
    readonly correlationIdObserved: (correlationId: string) => void;
}

/**
 * 认证设备并把提醒动作命令序列化为带心跳的 SSE 响应。
 * @param options 当前 HTTP 连接、Gateway 运行时与脱敏观测依赖。
 * @returns SSE 流结束或客户端断开后兑现的 Promise。
 */
export async function streamReminderActions(options: ReminderActionStreamHttpOptions): Promise<void> {
    const reminderType = options.url.searchParams.get('reminderType');
    const reminderTriggerId = options.url.searchParams.get('reminderTriggerId');
    if (reminderType !== 'strong' || reminderTriggerId === null || reminderTriggerId.trim() === '') {
        throw new TypeError('Invalid action stream scope');
    }
    const controller = new AbortController();
    options.request.once('aborted', () => controller.abort());
    options.response.once('close', () => controller.abort());
    const lastEventId = singleHeader(options.request.headers['last-event-id']);
    const events = await options.runtime.actionStreamApi.connect({
        authorization: options.request.headers.authorization ?? '',
        deviceId: unsafeId<DeviceId>(decodePathSegment(options.encodedDeviceId)),
        reminderType,
        reminderTriggerId: unsafeId<ReminderTriggerId>(reminderTriggerId),
        ...(lastEventId === undefined ? {} : { lastEventId: unsafeId<ActionId>(lastEventId) }),
        signal: controller.signal,
    });
    options.response.writeHead(200, { ...SSE_RESPONSE_HEADERS, Connection: 'keep-alive' });
    options.response.flushHeaders();
    const heartbeat = setInterval(() => {
        if (!options.response.destroyed && !options.response.writableNeedDrain) {
            options.response.write(': heartbeat\n\n');
        }
    }, SSE_HEARTBEAT_INTERVAL_SECONDS * 1000);
    heartbeat.unref();
    try {
        for await (const event of events) {
            options.correlationIdObserved(event.data.correlationId);
            const written = await writeWithBackpressure(
                options.response,
                `id: ${event.id}\nevent: ${event.event}\ndata: ${JSON.stringify(event.data)}\n\n`,
            );
            if (!written) break;
            safeLog(options.logger, {
                level: 'info',
                event: 'action.stream.sent',
                requestId: options.requestId,
                correlationId: event.data.correlationId,
                actionId: event.id,
            });
        }
    } finally {
        clearInterval(heartbeat);
        options.response.end();
    }
}

function writeWithBackpressure(response: ServerResponse, chunk: string): Promise<boolean> {
    if (response.destroyed || response.writableEnded) return Promise.resolve(false);
    if (response.write(chunk)) return Promise.resolve(true);
    return new Promise((resolve) => {
        let settled = false;
        const finish = (written: boolean): void => {
            if (settled) return;
            settled = true;
            response.off('drain', onDrain);
            response.off('close', onClose);
            resolve(written);
        };
        const onDrain = (): void => finish(true);
        const onClose = (): void => finish(false);
        response.once('drain', onDrain);
        response.once('close', onClose);
        if (response.destroyed || response.writableEnded) finish(false);
    });
}

function singleHeader(value: string | readonly string[] | undefined): string | undefined {
    return typeof value === 'string' ? value : value?.[0];
}

function decodePathSegment(value: string): string {
    try {
        return decodeURIComponent(value);
    } catch {
        throw new TypeError('Invalid encoded device id');
    }
}

function safeLog(logger: GatewayLogger, entry: Parameters<GatewayLogger['log']>[0]): void {
    try {
        logger.log(entry);
    } catch {
        // Logging failures must never change SSE behavior.
    }
}
