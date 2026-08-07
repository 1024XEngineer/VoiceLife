import { Context } from '@koishijs/core';

import type { ImGatewayDependencies, ImGatewayRuntime } from './create-im-gateway.js';
import { createImGateway } from './create-im-gateway.js';
import { KoishiChannelAdapter, KoishiContextBotFacade } from '../infrastructure/koishi/koishi-channel-adapter.js';
import { VoiceLifeKoishiPlugin } from '../infrastructure/koishi/voicelife-plugin.js';
import { SseActionCommandHub } from '../infrastructure/sse/sse-action-command-hub.js';
import type { ActionCommandStreamPort, PlatformCapabilityPort } from '../ports/external.js';
import { ImGatewayError } from '../shared/errors.js';

/** 真实 Koishi Runtime 与 Gateway Application 的同进程组合配置。 */
export interface KoishiGatewayOptions {
    readonly context?: Context;
    readonly dependencies: Omit<ImGatewayDependencies, 'actionStream' | 'imChannel'> & {
        readonly actionStream?: ActionCommandStreamPort;
    };
    readonly capabilities: readonly PlatformCapabilityPort[];
    /**
     * 解密仅供 Koishi 当前发送使用的外部用户标识。
     * @param ciphertext 持久化的受保护外部身份。
     * @returns Koishi Bot 使用的平台用户标识。
     */
    revealExternalUserId(ciphertext: string): Promise<string>;
}

/** 已装配且托管 Koishi 生命周期的 Gateway Runtime。 */
export interface KoishiGatewayRuntime {
    readonly context: Context;
    readonly runtime: ImGatewayRuntime;
    readonly actionStream: ActionCommandStreamPort;
    /**
     * 启动插件与真实 Koishi Context；重复调用不会重复注册监听器。
     * @returns Runtime 就绪后兑现的 Promise。
     */
    start(): Promise<void>;
    /**
     * 停止插件与 Koishi Context；重复调用不会重复释放资源。
     * @returns Runtime 停止后兑现的 Promise。
     */
    close(): Promise<void>;
}

/**
 * 组合真实 Koishi Runtime、Channel Adapter、SSE Hub 与 Gateway Application。
 * @param options Gateway 外部端口、平台能力和可选 Koishi Context。
 * @returns 托管 Koishi 生命周期的同进程 Runtime。
 */
export function createKoishiGatewayRuntime(options: KoishiGatewayOptions): KoishiGatewayRuntime {
    const capabilities = validateCapabilities(options.capabilities);
    const context = options.context ?? new Context();
    const actionStream = options.dependencies.actionStream ?? new SseActionCommandHub();
    const channel = new KoishiChannelAdapter({
        unitOfWork: options.dependencies.unitOfWork,
        bot: new KoishiContextBotFacade(context),
        revealExternalUserId: options.revealExternalUserId,
    });
    const runtime = createImGateway({
        ...options.dependencies,
        actionStream,
        imChannel: channel,
    });
    const plugins = capabilities.map(
        (capability) => new VoiceLifeKoishiPlugin(context, capability, runtime.application.platformEvents),
    );
    let started = false;
    let closed = false;
    let startPromise: Promise<void> | undefined;
    let closePromise: Promise<void> | undefined;

    return {
        context,
        runtime,
        actionStream,
        async start(): Promise<void> {
            if (closed) throw new ImGatewayError('invalid_transition', 'A closed Koishi Gateway cannot restart');
            if (started) return;
            if (startPromise !== undefined) return startPromise;
            const attempt = startKoishiContext(context, plugins);
            startPromise = attempt;
            try {
                await attempt;
                started = true;
            } finally {
                if (startPromise === attempt) startPromise = undefined;
            }
        },
        async close(): Promise<void> {
            if (closePromise !== undefined) return closePromise;
            closed = true;
            for (const plugin of plugins) plugin.stop();
            const attempt = (async (): Promise<void> => {
                try {
                    await startPromise;
                } catch {
                    // start() owns and reports startup failures; close() still completes cleanup.
                }
                for (const plugin of plugins) plugin.stop();
                if (started) {
                    await context.stop();
                    started = false;
                }
            })();
            closePromise = attempt;
            return attempt;
        },
    };
}

function validateCapabilities(capabilities: readonly PlatformCapabilityPort[]): readonly PlatformCapabilityPort[] {
    const platforms = new Set<PlatformCapabilityPort['platform']>();
    for (const capability of capabilities) {
        if (platforms.has(capability.platform)) {
            throw new ImGatewayError(
                'invalid_contract',
                `Only one platform capability may be registered for ${capability.platform}`,
            );
        }
        platforms.add(capability.platform);
    }
    return capabilities;
}

async function startKoishiContext(context: Context, plugins: readonly VoiceLifeKoishiPlugin[]): Promise<void> {
    for (const plugin of plugins) plugin.start();
    try {
        await context.start();
    } catch (error) {
        for (const plugin of plugins) plugin.stop();
        throw error;
    }
}
