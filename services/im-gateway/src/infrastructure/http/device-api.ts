import type { ActionId, DeviceId, PairingSessionId, ReminderTriggerId } from '../../contracts/ids.js';
import type {
    CreatedPairingSessionResponse,
    NotificationSubmission,
    PairingSessionStatus,
    ReminderActionCommand,
    ReminderType,
} from '../../contracts/device-gateway.js';
import {
    parseCreatePairingSessionRequest,
    parseNotificationIntent,
    parseReminderActionResult,
    parseScheduleReceiptIntent,
    parseScheduleQueryResultIntent,
    parseVoiceReminderActionStatus,
} from '../../contracts/device-gateway-parser.js';
import type { ActionApplication, NotificationApplication, PairingApplication } from '../../application/api.js';
import type { ImAction, PairingSession } from '../../domain/models.js';
import type { ActionCommandStreamPort, DeviceAuthenticationPort } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';

/** 设备侧 HTTPS 与 SSE 接口使用的稳定路由。 */
export const DEVICE_API_ROUTES = {
    pairingSessions: '/v1/im/pairing-sessions',
    pairingSession: '/v1/im/pairing-sessions/:pairingSessionId',
    scheduleReceipts: '/v1/im/schedule-receipts',
    scheduleQueryResults: '/v1/im/schedule-query-results',
    notifications: '/v1/im/notifications',
    voiceReminderActionStatuses: '/v1/im/reminder-action-statuses',
    reminderActionResults: '/v1/devices/:deviceId/reminder-actions/:commandId/result',
    reminderActionStream: '/v1/devices/:deviceId/reminder-actions/stream',
} as const;

/** 四类跨模块契约对应的规范传输方式。 */
export const DEVICE_API_ENDPOINTS = {
    scheduleReceipt: {
        method: 'POST',
        path: DEVICE_API_ROUTES.scheduleReceipts,
        transport: 'https',
    },
    scheduleQueryResult: {
        method: 'POST',
        path: DEVICE_API_ROUTES.scheduleQueryResults,
        transport: 'https',
    },
    notification: {
        method: 'POST',
        path: DEVICE_API_ROUTES.notifications,
        transport: 'https',
    },
    voiceReminderActionStatus: {
        method: 'POST',
        path: '/v1/im/reminder-action-statuses',
        transport: 'https',
    },
    reminderActionCommand: {
        method: 'GET',
        path: DEVICE_API_ROUTES.reminderActionStream,
        transport: 'sse',
    },
    reminderActionResult: {
        method: 'POST',
        path: DEVICE_API_ROUTES.reminderActionResults,
        transport: 'https',
    },
} as const;

/** 已携带设备授权和幂等键的意图请求。 */
export interface AuthenticatedIntentRequest {
    readonly authorization: string;
    readonly idempotencyKey: string;
    readonly body: unknown;
}

function publicPairingSession(session: PairingSession): PairingSessionStatus {
    return {
        id: session.id,
        ...(session.userId === undefined ? {} : { userId: session.userId }),
        deviceId: session.deviceId,
        ...(session.allowedPlatforms === undefined ? {} : { allowedPlatforms: session.allowedPlatforms }),
        status: session.status,
        expiresAt: session.expiresAt,
        createdAt: session.createdAt,
        ...(session.confirmedAt === undefined ? {} : { confirmedAt: session.confirmedAt }),
    };
}

/** 面向设备接口的框架无关 HTTP 控制器。 */
export class DeviceIntentController {
    /**
     * 创建设备侧 HTTP 控制器。
     * @param notifications 通知受理服务。
     * @param actions 提醒动作服务。
     * @param authentication 设备认证端口。
     * @param pairing 配对服务。
     */
    public constructor(
        private readonly notifications: NotificationApplication,
        private readonly actions: ActionApplication,
        private readonly authentication: DeviceAuthenticationPort,
        private readonly pairing: PairingApplication,
    ) {}

    /**
     * 认证设备并创建配对会话。
     * @param input 授权信息与配对参数。
     * @returns 新会话及展示码。
     */
    public async postPairingSession(input: {
        readonly authorization: string;
        readonly body: unknown;
    }): Promise<CreatedPairingSessionResponse> {
        const body = parseCreatePairingSessionRequest(input.body);
        const principal = await this.authentication.authenticate(input.authorization);
        if (principal.deviceId !== body.deviceId) {
            throw new ImGatewayError('invalid_transition', 'Device token is not bound to the requested deviceId');
        }
        if (body.userId !== undefined && body.userId !== principal.userId) {
            throw new ImGatewayError('invalid_transition', 'Device token is not bound to the requested userId');
        }
        const created = await this.pairing.create({ ...body, userId: principal.userId });
        return { session: publicPairingSession(created.session), displayCode: created.displayCode };
    }

    /**
     * 认证设备并查询属于该设备的配对会话。
     * @param input 授权信息与配对会话标识。
     * @returns 可见的配对会话，不存在或不属于设备时返回 undefined。
     */
    public async getPairingSession(input: {
        readonly authorization: string;
        readonly pairingSessionId: PairingSessionId;
    }): Promise<PairingSessionStatus | undefined> {
        const principal = await this.authentication.authenticate(input.authorization);
        const session = await this.pairing.find(input.pairingSessionId);
        if (session === undefined || session.deviceId !== principal.deviceId) {
            return undefined;
        }
        return publicPairingSession(session);
    }

    /**
     * 认证并受理日程操作回执请求。
     * @param input 带授权和幂等键的请求。
     * @returns 投递受理结果。
     */
    public async postScheduleReceipt(input: AuthenticatedIntentRequest): Promise<NotificationSubmission> {
        const body = parseScheduleReceiptIntent(input.body);
        await this.authenticateDevice(input.authorization, body.deviceId, body.userId);
        this.assertIdempotencyKey(input.idempotencyKey, body.eventId);
        return this.notifications.submitScheduleReceipt(body);
    }

    /**
     * 认证并受理完整日程查询结果。
     * @param input 带授权和幂等键的请求。
     * @returns 投递受理结果。
     */
    public async postScheduleQueryResult(input: AuthenticatedIntentRequest): Promise<NotificationSubmission> {
        const body = parseScheduleQueryResultIntent(input.body);
        await this.authenticateDevice(input.authorization, body.deviceId, body.userId);
        this.assertIdempotencyKey(input.idempotencyKey, body.businessEventId);
        return this.notifications.submitScheduleQueryResult(body);
    }

    /**
     * 认证并受理提醒通知请求。
     * @param input 带授权和幂等键的请求。
     * @returns 投递受理结果。
     */
    public async postNotification(input: AuthenticatedIntentRequest): Promise<NotificationSubmission> {
        const body = parseNotificationIntent(input.body);
        await this.authenticateDevice(input.authorization, body.recipient.deviceId, body.recipient.userId);
        this.assertIdempotencyKey(input.idempotencyKey, body.businessEventId);
        return this.notifications.submitNotification(body);
    }

    /**
     * 认证设备并记录提醒动作执行结果。
     * @param input 路径范围、授权信息与结果载荷。
     * @returns 归并结果后的动作记录。
     */
    public async postReminderActionResult(input: {
        readonly authorization: string;
        readonly deviceId: DeviceId;
        readonly commandId: ActionId;
        readonly body: unknown;
    }): Promise<ImAction> {
        const body = parseReminderActionResult(input.body);
        const principal = await this.authentication.authenticate(input.authorization);
        if (principal.deviceId !== input.deviceId) {
            throw new ImGatewayError('invalid_transition', 'Device principal does not match the result path');
        }
        return this.actions.recordResult(input.commandId, input.deviceId, body);
    }

    /**
     * 认证并接收设备语音直接消费后的状态事实。
     * @param input 带设备授权和状态事实的请求。
     * @returns 归并结果后的动作记录。
     */
    public async postVoiceReminderActionStatus(input: {
        readonly authorization: string;
        readonly idempotencyKey: string;
        readonly body: unknown;
    }): Promise<readonly ImAction[]> {
        const body = parseVoiceReminderActionStatus(input.body);
        const principal = await this.authentication.authenticate(input.authorization);
        if (principal.deviceId !== body.deviceId) {
            throw new ImGatewayError('invalid_transition', 'Device principal does not match the status body');
        }
        this.assertIdempotencyKey(input.idempotencyKey, body.eventId);
        return this.actions.recordVoiceStatus(body);
    }

    private async authenticateDevice(
        authorization: string,
        expectedDeviceId: DeviceId,
        expectedUserId?: string,
    ): Promise<void> {
        const principal = await this.authentication.authenticate(authorization);
        if (
            principal.deviceId !== expectedDeviceId ||
            (expectedUserId !== undefined && principal.userId !== expectedUserId)
        ) {
            throw new ImGatewayError('invalid_transition', 'Device principal does not match the intent body');
        }
    }

    private assertIdempotencyKey(idempotencyKey: string, businessEventId: string): void {
        if (idempotencyKey !== businessEventId) {
            throw new ImGatewayError('duplicate_event', 'Idempotency-Key must equal the contract business event ID');
        }
    }
}

/** 由 Koishi Server 路由序列化为 SSE 帧的动作事件。 */
export interface ReminderActionSseEvent {
    readonly id: ActionId;
    readonly event: 'reminder.action';
    readonly data: ReminderActionCommand;
}

/** 认证设备并合并待处理回放与实时命令的 SSE 控制器。 */
export class ReminderActionStreamController {
    /**
     * 创建设备动作 SSE 控制器。
     * @param stream 动作命令流端口。
     * @param authentication 设备认证端口。
     * @param actions 提醒动作服务。
     */
    public constructor(
        private readonly stream: ActionCommandStreamPort,
        private readonly authentication: DeviceAuthenticationPort,
        private readonly actions: ActionApplication,
    ) {}

    /**
     * 认证设备并连接带持久化回放的动作命令流。
     * @param input 设备、提醒窗口、游标与取消信号。
     * @returns 可序列化为 SSE 帧的异步事件流。
     */
    public async connect(input: {
        readonly authorization: string;
        readonly deviceId: DeviceId;
        readonly reminderType: ReminderType;
        readonly reminderTriggerId: ReminderTriggerId;
        readonly lastEventId?: ActionId;
        readonly signal?: AbortSignal;
    }): Promise<AsyncIterable<ReminderActionSseEvent>> {
        if (input.reminderType !== 'strong') {
            throw new ImGatewayError('invalid_contract', 'Only strong reminders can establish an action stream');
        }
        const principal = await this.authentication.authenticate(input.authorization);
        if (principal.deviceId !== input.deviceId) {
            throw new ImGatewayError('invalid_transition', 'Device token is not bound to the requested deviceId');
        }
        await this.actions.expireDue();
        const expiresAt = await this.actions.resolveActionWindow(input.deviceId, input.reminderTriggerId);
        const live = this.stream.subscribe({
            deviceId: input.deviceId,
            reminderTriggerId: input.reminderTriggerId,
            expiresAt,
            ...(input.lastEventId === undefined ? {} : { lastEventId: input.lastEventId }),
            ...(input.signal === undefined ? {} : { signal: input.signal }),
        });
        let replay: readonly ReminderActionCommand[];
        try {
            replay = await this.actions.replayPending(input.deviceId, input.reminderTriggerId, input.lastEventId);
        } catch (error) {
            await live[Symbol.asyncIterator]().return?.();
            throw error;
        }
        return markCommandsProcessing(concatenateCommands(replay, live), this.actions);
    }
}

async function* concatenateCommands(
    replay: readonly ReminderActionCommand[],
    live: AsyncIterable<ReminderActionCommand>,
): AsyncIterable<ReminderActionCommand> {
    const seen = new Set<ActionId>();
    for (const command of replay) {
        seen.add(command.commandId);
        yield command;
    }
    for await (const command of live) {
        if (seen.has(command.commandId)) continue;
        seen.add(command.commandId);
        yield command;
    }
}

async function* markCommandsProcessing(
    commands: AsyncIterable<ReminderActionCommand>,
    actions: ActionApplication,
): AsyncIterable<ReminderActionSseEvent> {
    for await (const command of commands) {
        await actions.markProcessing(command.commandId, command.deviceId, command.reminderTriggerId);
        yield {
            id: command.commandId,
            event: 'reminder.action',
            data: command,
        };
    }
}

/** SSE 响应必须设置的协议与代理头。 */
export const SSE_RESPONSE_HEADERS = {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache',
    'X-Accel-Buffering': 'no',
} as const;

/** SSE 连接的心跳发送间隔，单位为秒。 */
export const SSE_HEARTBEAT_INTERVAL_SECONDS = 20;
