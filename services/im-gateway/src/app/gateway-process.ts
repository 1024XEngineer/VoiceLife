import { createKoishiGatewayRuntime, type KoishiGatewayRuntime } from './create-koishi-gateway.js';
import { Context } from '@koishijs/core';
import { unsafeId, type ChannelAccountId } from '../contracts/ids.js';
import type { ChannelAccount } from '../domain/models.js';
import { DeliveryOutboxWorker } from '../infrastructure/delivery-outbox-worker.js';
import { ChannelAdapterRegistry } from '../infrastructure/channel-adapter-registry.js';
import {
    type GatewayLogger,
    startGatewayHttpServer,
    type StartedGatewayHttpServer,
} from '../infrastructure/http/gateway-http-server.js';
import { JsonLineGatewayLogger } from '../infrastructure/observability/json-line-logger.js';
import { PostgresImUnitOfWork } from '../infrastructure/persistence/postgres.js';
import {
    CapabilityChannelHealthPort,
    DirectConversationResolver,
    SystemClock,
} from '../infrastructure/production-support.js';
import { AesGcmActionTokenPort } from '../infrastructure/security/aes-gcm-action-token.js';
import {
    AesGcmExternalIdentityProtector,
    DatabaseDeviceAuthenticationPort,
    HmacPairingCodePort,
    UuidIdGenerator,
} from '../infrastructure/security/production-ports.js';
import { WechatOfficialAdapter } from '../infrastructure/wechat/wechat-official-adapter.js';
import { WechatOfficialKoishiBot } from '../infrastructure/koishi/wechat-official-koishi-bot.js';

export {
    startGatewayHttpServer,
    type GatewayHttpServerOptions,
    type GatewayLogEntry,
    type GatewayLogger,
    type StartedGatewayHttpServer,
} from '../infrastructure/http/gateway-http-server.js';

/** 生产进程可读取的环境变量集合。 */
export type GatewayEnvironment = Readonly<Record<string, string | undefined>>;

const PUBLIC_EXAMPLE_SECRET_VALUES = new Set([
    'replace-with-at-least-32-random-bytes',
    'replace-with-a-distinct-32-byte-random-secret',
]);

/** 只携带环境变量名与公开约束、不携带配置值的生产配置错误。 */
export class GatewayConfigurationError extends Error {
    /** @param message 可安全写入启动日志的配置约束说明。 */
    public constructor(message: string) {
        super(message);
        this.name = 'GatewayConfigurationError';
    }
}

/** 生产进程经过校验的微信公众号配置。 */
export interface GatewayWechatConfiguration {
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
}

/** 生产进程经过校验且仅驻留内存的配置。 */
export interface GatewayConfiguration {
    readonly host: string;
    readonly port: number;
    readonly databaseUrl: string;
    readonly actionTokenSecret: string;
    readonly identitySecret: string;
    readonly wechat: GatewayWechatConfiguration;
}

/** 已启动且托管全部依赖生命周期的生产 Gateway 进程。 */
export interface StartedGatewayProcess {
    readonly origin: string;
    /** @returns HTTP、Koishi 与 PostgreSQL 全部停止后兑现的 Promise。 */
    close(): Promise<void>;
}

/**
 * 从环境/Secret 引用解析生产配置，错误只包含变量名而不包含变量值。
 * @param environment 进程环境变量。
 * @returns 完整且通过格式校验的生产配置。
 */
export function readGatewayConfiguration(environment: GatewayEnvironment): GatewayConfiguration {
    const host = environment.GATEWAY_HOST?.trim() || '0.0.0.0';
    const rawPort = environment.GATEWAY_PORT?.trim() || '3000';
    const port = Number(rawPort);
    if (!/^\d{1,5}$/u.test(rawPort) || !Number.isSafeInteger(port) || port < 0 || port > 65_535) {
        throw new GatewayConfigurationError('GATEWAY_PORT must be a valid TCP port');
    }
    if ((environment.WECHAT_WEBHOOK_MODE?.trim() || 'plain') !== 'plain') {
        throw new GatewayConfigurationError('WECHAT_WEBHOOK_MODE must be plain for the current adapter');
    }
    const databaseUrl = resolveDatabaseConnectionUrl(environment);
    const actionUiBaseUrl = requiredEnvironment(environment, 'WECHAT_ACTION_UI_BASE_URL');
    assertActionUiBaseUrl(actionUiBaseUrl);
    const expectedToUserName = requiredEnvironment(environment, 'WECHAT_EXPECTED_TO_USERNAME');
    if (!/^gh_[A-Za-z0-9_-]+$/u.test(expectedToUserName)) {
        throw new GatewayConfigurationError('WECHAT_EXPECTED_TO_USERNAME must be the gh_ prefixed original account ID');
    }
    const actionTokenSecret = requiredEnvironment(environment, 'ACTION_TOKEN_SECRET');
    assertProductionSecret(actionTokenSecret, 'ACTION_TOKEN_SECRET', 32);
    const identitySecret = environment.IDENTITY_SECRET?.trim() || actionTokenSecret;
    assertProductionSecret(identitySecret, 'IDENTITY_SECRET', 32);
    return {
        host,
        port,
        databaseUrl,
        actionTokenSecret,
        identitySecret,
        wechat: {
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
        },
    };
}

/**
 * 使用 PostgreSQL、Koishi、SSE Hub、微信 Adapter 与生产安全端口启动 Gateway。
 * @param environment 只通过环境或容器 Secret 注入的进程配置。
 * @param logger 可替换的脱敏结构化日志器。
 * @param options 可替换的基础设施依赖，仅用于测试或受控部署。
 * @returns 已监听并可优雅关闭的生产进程。
 */
export async function startConfiguredGatewayProcess(
    environment: GatewayEnvironment,
    logger: GatewayLogger = new JsonLineGatewayLogger(),
): Promise<StartedGatewayProcess> {
    const config = readGatewayConfiguration(environment);
    const processLogger = nonThrowingLogger(logger);
    const unitOfWork = new PostgresImUnitOfWork(config.databaseUrl);
    let koishi: KoishiGatewayRuntime | undefined;
    let deliveryWorker: DeliveryOutboxWorker | undefined;
    let http: StartedGatewayHttpServer | undefined;
    try {
        await unitOfWork.migrate();
        const clock = new SystemClock();
        const identities = new AesGcmExternalIdentityProtector(config.identitySecret);
        const channelAccountId = unsafeId<ChannelAccountId>(config.wechat.channelAccountId);
        const adapter = new WechatOfficialAdapter({
            channelAccountId,
            token: config.wechat.webhookToken,
            expectedToUserName: config.wechat.expectedToUserName,
            outbound: {
                appId: config.wechat.appId,
                appSecret: config.wechat.appSecret,
                templateId: config.wechat.templateId,
                templateFields: config.wechat.templateFields,
                queryTemplateId: config.wechat.queryTemplateId,
                queryTemplateFields: config.wechat.queryTemplateFields,
                displayTimeZone: config.wechat.displayTimeZone,
                actionUiBaseUrl: config.wechat.actionUiBaseUrl,
                revealExternalUserId: (ciphertext) => identities.reveal(ciphertext),
            },
        });
        const channelAdapters = new ChannelAdapterRegistry([{ accountId: channelAccountId, adapter }]);
        const context = new Context();
        const koishiBotId = `wechat:${channelAccountId}`;
        const wechatBot = new WechatOfficialKoishiBot(context, {
            koishiBotId,
            selfId: channelAccountId,
            transport: adapter,
        });
        koishi = createKoishiGatewayRuntime({
            context,
            dependencies: {
                unitOfWork,
                actionTokens: new AesGcmActionTokenPort(config.actionTokenSecret),
                authentication: new DatabaseDeviceAuthenticationPort(unitOfWork),
                channelCapabilities: channelAdapters,
                channelHealth: new CapabilityChannelHealthPort(
                    adapter,
                    clock,
                    (account) => account.koishiBotId === wechatBot.sid && wechatBot.isActive,
                ),
                conversations: new DirectConversationResolver(),
                deliveryRenderer: channelAdapters,
                imChannel: channelAdapters,
                pairingCodes: new HmacPairingCodePort(config.identitySecret),
                identityProtector: identities,
                clock,
                ids: new UuidIdGenerator(channelAccountId),
                wechatAdapter: adapter,
                actionUiObserver: {
                    submitted: (command) => {
                        processLogger.log({
                            level: 'info',
                            event: 'action.submitted',
                            correlationId: command.correlationId,
                            actionId: command.commandId,
                        });
                    },
                },
            },
            capabilities: [adapter],
            revealExternalUserId: (ciphertext) => identities.reveal(ciphertext),
        });
        await ensureConfiguredChannel(koishi.runtime, channelAccountId, config.wechat);
        await koishi.start();
        deliveryWorker = new DeliveryOutboxWorker({
            unitOfWork,
            dispatch: koishi.runtime.application.deliveryDispatch,
            clock,
            logger: processLogger,
        });
        deliveryWorker.start();
        http = await startGatewayHttpServer({
            host: config.host,
            port: config.port,
            runtime: koishi.runtime,
            logger: processLogger,
            deliveryAvailable: () => deliveryWorker!.wake(),
            healthCheck: async () => {
                const account = await koishi!.runtime.application.channels.find(channelAccountId);
                if (account === undefined || account.status !== 'active') throw new Error('channel unavailable');
                const health = await koishi!.runtime.application.channels.health(channelAccountId);
                if (health.status !== 'healthy') throw new Error('channel unavailable');
                return { status: 'ok' };
            },
        });
        processLogger.log({ level: 'info', event: 'gateway.started' });
        let closePromise: Promise<void> | undefined;
        return {
            origin: http.origin,
            close(): Promise<void> {
                closePromise ??= closeGateway(http!, deliveryWorker!, koishi!, unitOfWork, processLogger);
                return closePromise;
            },
        };
    } catch (error) {
        if (http !== undefined) await http.close().catch(() => undefined);
        if (deliveryWorker !== undefined) await deliveryWorker.close().catch(() => undefined);
        if (koishi !== undefined) await koishi.close().catch(() => undefined);
        await unitOfWork.close().catch(() => undefined);
        throw error;
    }
}

function nonThrowingLogger(logger: GatewayLogger): GatewayLogger {
    return {
        log(entry): void {
            try {
                logger.log(entry);
            } catch {
                // Observability failures must not affect process lifecycle.
            }
        },
    };
}

async function ensureConfiguredChannel(
    runtime: KoishiGatewayRuntime['runtime'],
    channelAccountId: ChannelAccountId,
    wechat: GatewayWechatConfiguration,
): Promise<void> {
    const existing = await runtime.application.channels.find(channelAccountId);
    if (existing === undefined) {
        await runtime.application.channels.register({
            platform: 'wechat_official',
            tenantExternalId: wechat.expectedToUserName,
            koishiBotId: `wechat:${channelAccountId}`,
            credentialRef: 'secret://env/WECHAT_APP_SECRET',
            connectionMode: 'webhook',
        });
        return;
    }
    assertConfiguredChannel(existing, wechat);
}

function assertConfiguredChannel(account: ChannelAccount, wechat: GatewayWechatConfiguration): void {
    if (
        account.platform !== 'wechat_official' ||
        account.tenantExternalId !== wechat.expectedToUserName ||
        account.credentialRef !== 'secret://env/WECHAT_APP_SECRET' ||
        account.koishiBotId !== `wechat:${account.id}` ||
        account.connectionMode !== 'webhook' ||
        account.status !== 'active'
    ) {
        throw new GatewayConfigurationError(
            'Configured WeChat channel account conflicts with the persisted deployment metadata',
        );
    }
}

async function closeGateway(
    http: StartedGatewayHttpServer,
    deliveryWorker: DeliveryOutboxWorker,
    koishi: KoishiGatewayRuntime,
    unitOfWork: PostgresImUnitOfWork,
    logger: GatewayLogger,
): Promise<void> {
    const errors: unknown[] = [];
    for (const close of [
        () => http.close(),
        () => deliveryWorker.close(),
        () => koishi.close(),
        () => unitOfWork.close(),
    ]) {
        try {
            await close();
        } catch (error) {
            errors.push(error);
        }
    }
    logger.log({ level: errors.length === 0 ? 'info' : 'error', event: 'gateway.stopped' });
    if (errors.length > 0) throw new AggregateError(errors, 'Gateway shutdown failed');
}

function requiredEnvironment(environment: GatewayEnvironment, name: string): string {
    const value = environment[name]?.trim();
    if (value === undefined || value === '') throw new GatewayConfigurationError(`${name} is required`);
    return value;
}

function assertProductionSecret(value: string, name: string, minimum: number): void {
    if (Buffer.byteLength(value, 'utf8') < minimum) {
        throw new GatewayConfigurationError(`${name} must contain at least ${String(minimum)} bytes`);
    }
    if (PUBLIC_EXAMPLE_SECRET_VALUES.has(value)) {
        throw new GatewayConfigurationError(`${name} must not use the public example value`);
    }
}

function templateField(environment: GatewayEnvironment, name: string): string {
    const value = requiredEnvironment(environment, name);
    if (!/^[A-Za-z][A-Za-z0-9_]{0,63}$/u.test(value)) {
        throw new GatewayConfigurationError(`${name} must be a valid WeChat template field name`);
    }
    return value;
}

function displayTimeZone(environment: GatewayEnvironment): string {
    const value = environment.WECHAT_DISPLAY_TIME_ZONE?.trim() || 'Asia/Shanghai';
    try {
        new Intl.DateTimeFormat('en-US', { timeZone: value }).format(0);
    } catch {
        throw new GatewayConfigurationError('WECHAT_DISPLAY_TIME_ZONE must be a valid IANA time zone');
    }
    return value;
}

function assertDatabaseUrl(value: string): void {
    try {
        const url = new URL(value);
        if ((url.protocol !== 'postgres:' && url.protocol !== 'postgresql:') || url.hostname === '') {
            throw new Error('invalid PostgreSQL URL');
        }
    } catch {
        throw new GatewayConfigurationError('DATABASE_URL must be a PostgreSQL connection URL');
    }
}

/**
 * 校验数据库 URL，并在容器运行时安全应用可选主机名覆盖。
 * @param environment Gateway 或设备 CLI 的进程环境。
 * @returns 可直接交给 PostgreSQL 客户端的连接 URL。
 */
export function resolveDatabaseConnectionUrl(environment: GatewayEnvironment): string {
    const value = requiredEnvironment(environment, 'DATABASE_URL');
    assertDatabaseUrl(value);
    const host = environment.DATABASE_HOST?.trim();
    if (host === undefined || host === '') return value;
    if (!/^(?:[A-Za-z0-9-]+\.)*[A-Za-z0-9-]+$/u.test(host)) {
        throw new GatewayConfigurationError('DATABASE_HOST must be a valid hostname');
    }
    const url = new URL(value);
    url.hostname = host;
    return url.toString();
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
        throw new GatewayConfigurationError('WECHAT_ACTION_UI_BASE_URL must be a public HTTPS Action UI base URL');
    }
}
