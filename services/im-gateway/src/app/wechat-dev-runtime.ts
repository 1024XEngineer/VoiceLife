import { randomUUID } from 'node:crypto';

import { createMockImGateway } from './create-im-gateway.js';
import type { NotificationIntent } from '../contracts/device-gateway.js';
import { unsafeId, type ChannelAccountId, type DeviceId, type UserId } from '../contracts/ids.js';
import type { Delivery } from '../domain/models.js';
import {
    startWechatDevHttpHarness,
    type StartedWechatDevHttpHarness,
    type WechatDevDeliverySnapshot,
} from '../infrastructure/http/wechat-dev-harness.js';
import { FixedClock, SequentialIdGenerator } from '../infrastructure/mock-support.js';
import { AesGcmActionTokenPort } from '../infrastructure/security/aes-gcm-action-token.js';
import { WechatOfficialAdapter } from '../infrastructure/wechat/wechat-official-adapter.js';
import type { Clock } from '../ports/external.js';
import type { IsoDateTime } from '../shared/types.js';

/** 微信开发 harness 可读取的环境变量集合。 */
export type WechatDevEnvironment = Readonly<Record<string, string | undefined>>;

/** 微信开发 harness 在自动化测试中允许替换的外部依赖。 */
export interface WechatDevRuntimeOverrides {
    readonly fetch?: typeof fetch;
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
    readonly actionUiBaseUrl: string;
    readonly openId: string;
    readonly deviceId: string;
    readonly deviceToken: string;
    readonly actionTokenSecret: string;
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
            actionUiBaseUrl: config.actionUiBaseUrl,
            revealExternalUserId,
            ...(overrides.fetch === undefined ? {} : { fetch: overrides.fetch }),
        },
    });
    const deviceId = unsafeId<DeviceId>(config.deviceId);
    const userId = unsafeId<UserId>('wechat-dev-user');
    const runtime = createMockImGateway(deviceId, new FixedClock(clock.now()), {
        clock,
        ids,
        actionTokens: new AesGcmActionTokenPort(config.actionTokenSecret),
        channelCapabilities: adapter,
        deliveryRenderer: adapter,
        imChannel: adapter,
        wechatAdapter: adapter,
    });

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

    return startWechatDevHttpHarness({
        host: config.host,
        port: config.port,
        deviceToken: config.deviceToken,
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
            return details === undefined
                ? undefined
                : {
                      ...deliverySnapshot(details.delivery),
                      attempts: details.attempts.length,
                      receipts: details.receipts.length,
                  };
        },
    });
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
        actionUiBaseUrl,
        openId: requiredEnvironment(environment, 'WECHAT_TEST_OPENID'),
        deviceId: requiredEnvironment(environment, 'DEVICE_ID'),
        deviceToken: requiredEnvironment(environment, 'DEVICE_TOKEN'),
        actionTokenSecret: requiredEnvironment(environment, 'ACTION_TOKEN_SECRET'),
    };
    if (Buffer.byteLength(config.deviceToken, 'utf8') < 24) {
        throw new Error('DEVICE_TOKEN must contain at least 24 bytes');
    }
    if (Buffer.byteLength(config.actionTokenSecret, 'utf8') < 32) {
        throw new Error('ACTION_TOKEN_SECRET must contain at least 32 bytes');
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

function revealExternalUserId(ciphertext: string): Promise<string> {
    const prefix = 'ciphertext:';
    if (!ciphertext.startsWith(prefix) || ciphertext.length === prefix.length) {
        return Promise.reject(new Error('WeChat development identity ciphertext is invalid'));
    }
    return Promise.resolve(ciphertext.slice(prefix.length));
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
