import { randomUUID } from 'node:crypto';

import { createImGateway, mockImGatewayPorts } from './create-im-gateway.js';
import type { NotificationIntent } from '../contracts/device-gateway.js';
import { unsafeId, type ChannelAccountId, type DeviceId } from '../contracts/ids.js';
import type { Delivery } from '../domain/models.js';
import {
    startWechatDevHttpHarness,
    type StartedWechatDevHttpHarness,
    type WechatDevDeliverySnapshot,
} from '../infrastructure/http/wechat-dev-harness.js';
import { FixedClock, SequentialIdGenerator } from '../infrastructure/mock-support.js';
import { PostgresImUnitOfWork } from '../infrastructure/persistence/postgres.js';
import { AesGcmActionTokenPort } from '../infrastructure/security/aes-gcm-action-token.js';
import {
    AesGcmExternalIdentityProtector,
    DatabaseDeviceAuthenticationPort,
    HmacPairingCodePort,
} from '../infrastructure/security/production-ports.js';
import { WechatOfficialAdapter } from '../infrastructure/wechat/wechat-official-adapter.js';
import type { Clock } from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';
import type { IsoDateTime } from '../shared/types.js';

/** 微信开发 harness 可读取的环境变量集合。 */
export type WechatDevEnvironment = Readonly<Record<string, string | undefined>>;

/** 微信开发 harness 在自动化测试中允许替换的外部依赖。 */
export interface WechatDevRuntimeOverrides {
    readonly fetch?: typeof fetch;
    /** 仅测试使用；生产 harness 始终创建并托管 PostgreSQL UoW。 */
    readonly unitOfWork?: ImUnitOfWork;
    /** 仅测试迁移失败清理；正常运行不得替换 PostgreSQL 工厂。 */
    readonly unitOfWorkFactory?: (databaseUrl: string) => ImUnitOfWork & {
        migrate(): Promise<void>;
        close(): Promise<void>;
    };
}

interface WechatDevConfiguration {
    readonly host: string;
    readonly port: number;
    readonly channelAccountId: string;
    readonly appId: string;
    readonly appSecret: string;
    readonly webhookToken: string;
    readonly expectedToUserName: string;
    readonly templateId: string;
    readonly templateFields: {
        readonly title: string;
        readonly body: string;
        readonly time: string;
    };
    readonly queryTemplateId: string;
    readonly queryTemplateFields: {
        readonly title: string;
        readonly body: string;
        readonly time: string;
    };
    readonly displayTimeZone: string;
    readonly actionUiBaseUrl: string;
    readonly openId: string;
    readonly databaseUrl: string;
    readonly deviceId: string;
    readonly actionTokenSecret: string;
    readonly identitySecret: string;
}

/**
 * 从环境变量装配并启动真实微信 Adapter 的开发 HTTP harness。
 * @param environment 本地 `.env` 提供的配置，不会被记录到日志。
 * @param overrides 仅供自动化测试替换微信 Fetch 的依赖。
 * @returns 已启动的本地联调监听器。
 */
export async function startConfiguredWechatDevHarness(
    environment: WechatDevEnvironment,
    overrides: WechatDevRuntimeOverrides = {},
): Promise<StartedWechatDevHttpHarness> {
    const config = readConfiguration(environment);
    const clock = new SystemClock();
    const identityProtector = new AesGcmExternalIdentityProtector(config.identitySecret);
    const ids = new DevHarnessIdGenerator(unsafeId<ChannelAccountId>(config.channelAccountId));
    const adapter = new WechatOfficialAdapter({
        channelAccountId: unsafeId<ChannelAccountId>(config.channelAccountId),
        token: config.webhookToken,
        expectedToUserName: config.expectedToUserName,
        outbound: {
            appId: config.appId,
            appSecret: config.appSecret,
            templateId: config.templateId,
            templateFields: config.templateFields,
            queryTemplateId: config.queryTemplateId,
            queryTemplateFields: config.queryTemplateFields,
            displayTimeZone: config.displayTimeZone,
            actionUiBaseUrl: config.actionUiBaseUrl,
            revealExternalUserId: (ciphertext) => identityProtector.reveal(ciphertext),
            ...(overrides.fetch === undefined ? {} : { fetch: overrides.fetch }),
        },
    });
    const deviceId = unsafeId<DeviceId>(config.deviceId);
    const ownedUnitOfWork = overrides.unitOfWork === undefined;
    const owned = ownedUnitOfWork
        ? (overrides.unitOfWorkFactory?.(config.databaseUrl) ?? new PostgresImUnitOfWork(config.databaseUrl))
        : undefined;
    const unitOfWork = overrides.unitOfWork ?? owned!;
    try {
        if (owned !== undefined) await owned.migrate();
        const device = await unitOfWork.transaction((tx) => tx.devices.findById(deviceId));
        if (device === undefined || device.status !== 'active') {
            throw new Error('WECHAT_DEV_DEVICE_ID must reference an active registered device');
        }
        const userId = device.userId;
        const runtime = createImGateway({
            ...mockImGatewayPorts(deviceId, new FixedClock(clock.now())),
            unitOfWork,
            clock,
            ids,
            authentication: new DatabaseDeviceAuthenticationPort(unitOfWork),
            actionTokens: new AesGcmActionTokenPort(config.actionTokenSecret),
            pairingCodes: new HmacPairingCodePort(config.identitySecret),
            identityProtector,
            channelCapabilities: adapter,
            deliveryRenderer: adapter,
            imChannel: adapter,
            wechatAdapter: adapter,
        });

        const existingChannel = await runtime.application.channels.find(
            unsafeId<ChannelAccountId>(config.channelAccountId),
        );
        if (existingChannel === undefined)
            await runtime.application.channels.register({
                platform: 'wechat_official',
                tenantExternalId: config.expectedToUserName,
                koishiBotId: 'wechat-dev-harness',
                credentialRef: 'secret://env/WECHAT_APP_SECRET',
                connectionMode: 'webhook',
            });
        const pairing = await runtime.application.pairing.create({ userId, deviceId });
        await runtime.application.pairing.confirm({
            displayCode: pairing.displayCode,
            channelAccountId: unsafeId<ChannelAccountId>(config.channelAccountId),
            externalUserId: config.openId,
        });

        const http = await startWechatDevHttpHarness({
            host: config.host,
            port: config.port,
            authentication: new DatabaseDeviceAuthenticationPort(unitOfWork),
            expectedDeviceId: deviceId,
            webhookApi: requiredWechatApi(runtime.wechatApi),
            actionUiPageApi: runtime.actionUiPageApi,
            sendTestNotification: async () => {
                const now = clock.now();
                const unique = randomUUID();
                const intent: NotificationIntent = {
                    schemaVersion: '1',
                    businessEventId: unsafeId(`wechat-dev-event-${unique}`),
                    correlationId: unsafeId(`wechat-dev-correlation-${unique}`),
                    kind: 'reminder_due',
                    recipient: { userId, deviceId },
                    scheduleId: unsafeId(`wechat-dev-schedule-${unique}`),
                    taskId: unsafeId(`wechat-dev-task-${unique}`),
                    instanceId: unsafeId(`wechat-dev-instance-${unique}`),
                    reminderTriggerId: unsafeId(`wechat-dev-trigger-${unique}`),
                    reminderType: 'strong',
                    content: {
                        title: 'VoiceLife 微信联调提醒',
                        body: `真实测试账号投递 ${now}`,
                    },
                    plannedAt: now,
                    triggerAt: now,
                    actions: [
                        { kind: 'command', type: 'acknowledge', label: '知道了' },
                        { kind: 'command', type: 'snooze', label: '推迟 10 分钟', params: { minutes: 10 } },
                    ],
                    occurredAt: now,
                };
                const submission = await runtime.application.notifications.submitNotification(intent);
                const pending = submission.deliveries[0];
                if (pending === undefined) throw new Error('WeChat development binding produced no delivery');
                const delivery = await runtime.application.deliveryDispatch.dispatch(pending.deliveryId);
                return deliverySnapshot(delivery);
            },
            inspectDelivery: async (deliveryId) => {
                const details = await runtime.application.deliveries.find(unsafeId(deliveryId));
                if (details === undefined) return undefined;
                const belongsToExpectedDevice = await unitOfWork.transaction(async (tx) => {
                    const binding = await tx.bindings.findById(details.delivery.bindingId);
                    return binding?.deviceId === deviceId;
                });
                return belongsToExpectedDevice
                    ? {
                          ...deliverySnapshot(details.delivery),
                          attempts: details.attempts.length,
                          receipts: details.receipts.length,
                      }
                    : undefined;
            },
        });
        return {
            origin: http.origin,
            close: async () => {
                try {
                    await http.close();
                } finally {
                    if (owned !== undefined) await owned.close();
                }
            },
        };
    } catch (error) {
        if (owned !== undefined) await owned.close().catch(() => undefined);
        throw error;
    }
}

function readConfiguration(environment: WechatDevEnvironment): WechatDevConfiguration {
    const host = environment.WECHAT_DEV_HOST?.trim() || '127.0.0.1';
    const rawPort = environment.WECHAT_DEV_PORT?.trim() || '3000';
    const port = Number(rawPort);
    if (!/^\d{1,5}$/u.test(rawPort) || !Number.isSafeInteger(port) || port < 0 || port > 65_535) {
        throw new Error('WECHAT_DEV_PORT must be a valid TCP port');
    }
    if ((environment.WECHAT_WEBHOOK_MODE?.trim() || 'plain') !== 'plain') {
        throw new Error('WECHAT_WEBHOOK_MODE must be plain for the current adapter');
    }
    const expectedToUserName = requiredEnvironment(environment, 'WECHAT_EXPECTED_TO_USERNAME');
    if (!/^gh_[A-Za-z0-9_-]+$/u.test(expectedToUserName)) {
        throw new Error('WECHAT_EXPECTED_TO_USERNAME must be the gh_ prefixed original account ID');
    }
    const actionUiBaseUrl = requiredEnvironment(environment, 'WECHAT_ACTION_UI_BASE_URL');
    assertActionUiBaseUrl(actionUiBaseUrl);
    const config: WechatDevConfiguration = {
        host,
        port,
        channelAccountId: requiredEnvironment(environment, 'WECHAT_CHANNEL_ACCOUNT_ID'),
        appId: requiredEnvironment(environment, 'WECHAT_APP_ID'),
        appSecret: requiredEnvironment(environment, 'WECHAT_APP_SECRET'),
        webhookToken: requiredEnvironment(environment, 'WECHAT_WEBHOOK_TOKEN'),
        expectedToUserName,
        templateId: requiredEnvironment(environment, 'WECHAT_TEMPLATE_ID'),
        templateFields: {
            title: templateField(environment, 'WECHAT_TEMPLATE_TITLE_FIELD'),
            body: templateField(environment, 'WECHAT_TEMPLATE_BODY_FIELD'),
            time: templateField(environment, 'WECHAT_TEMPLATE_TIME_FIELD'),
        },
        queryTemplateId: requiredEnvironment(environment, 'WECHAT_QUERY_TEMPLATE_ID'),
        queryTemplateFields: {
            title: templateField(environment, 'WECHAT_QUERY_TEMPLATE_TITLE_FIELD'),
            body: templateField(environment, 'WECHAT_QUERY_TEMPLATE_BODY_FIELD'),
            time: templateField(environment, 'WECHAT_QUERY_TEMPLATE_TIME_FIELD'),
        },
        displayTimeZone: displayTimeZone(environment),
        actionUiBaseUrl,
        openId: requiredEnvironment(environment, 'WECHAT_TEST_OPENID'),
        databaseUrl: requiredEnvironment(environment, 'DATABASE_URL'),
        deviceId: requiredEnvironment(environment, 'WECHAT_DEV_DEVICE_ID'),
        actionTokenSecret: requiredEnvironment(environment, 'ACTION_TOKEN_SECRET'),
        identitySecret: environment.IDENTITY_SECRET?.trim() || requiredEnvironment(environment, 'ACTION_TOKEN_SECRET'),
    };
    if (Buffer.byteLength(config.actionTokenSecret, 'utf8') < 32) {
        throw new Error('ACTION_TOKEN_SECRET must contain at least 32 bytes');
    }
    if (Buffer.byteLength(config.identitySecret, 'utf8') < 32) {
        throw new Error('IDENTITY_SECRET must contain at least 32 bytes');
    }
    return config;
}

function requiredEnvironment(environment: WechatDevEnvironment, name: string): string {
    const value = environment[name]?.trim();
    if (value === undefined || value === '') throw new Error(`${name} is required`);
    return value;
}

function templateField(environment: WechatDevEnvironment, name: string): string {
    const value = requiredEnvironment(environment, name);
    if (!/^[A-Za-z][A-Za-z0-9_]{0,63}$/u.test(value)) {
        throw new Error(`${name} must be a valid WeChat template field name`);
    }
    return value;
}

function displayTimeZone(environment: WechatDevEnvironment): string {
    const value = environment.WECHAT_DISPLAY_TIME_ZONE?.trim() || 'Asia/Shanghai';
    try {
        new Intl.DateTimeFormat('en-US', { timeZone: value }).format(0);
    } catch {
        throw new Error('WECHAT_DISPLAY_TIME_ZONE must be a valid IANA time zone');
    }
    return value;
}

function assertActionUiBaseUrl(value: string): void {
    try {
        const url = new URL(value);
        if (
            url.protocol !== 'https:' ||
            url.username !== '' ||
            url.password !== '' ||
            url.search !== '' ||
            url.hash !== '' ||
            !url.pathname.endsWith('/voicelife/reminder-actions')
        ) {
            throw new Error('invalid Action UI URL');
        }
    } catch {
        throw new Error('WECHAT_ACTION_UI_BASE_URL must be a public HTTPS Action UI base URL');
    }
}

function requiredWechatApi<T>(value: T | undefined): T {
    if (value === undefined) throw new Error('WeChat development runtime did not expose its webhook controller');
    return value;
}

function deliverySnapshot(delivery: Delivery): WechatDevDeliverySnapshot {
    return {
        deliveryId: delivery.id,
        status: delivery.status,
        ...(delivery.externalMessageId === undefined ? {} : { externalMessageId: delivery.externalMessageId }),
    };
}

class SystemClock implements Clock {
    public now(): IsoDateTime {
        return new Date().toISOString() as IsoDateTime;
    }

    public addMinutes(value: IsoDateTime, minutes: number): IsoDateTime {
        return new Date(Date.parse(value) + minutes * 60_000).toISOString() as IsoDateTime;
    }
}

class DevHarnessIdGenerator extends SequentialIdGenerator {
    public constructor(private readonly channelAccountId: ChannelAccountId) {
        super();
    }

    public override nextChannelAccountId(): ChannelAccountId {
        return this.channelAccountId;
    }
}
