import type { Context, Session } from '@koishijs/core';

import type { PlatformEventApplication } from '../../application/api.js';
import type { PlatformCapabilityPort } from '../../ports/external.js';

/** 处理已隔离的 Koishi 入站错误，供日志与监控接入。 */
export type VoiceLifeKoishiPluginErrorHandler = (error: unknown, session: Session) => void | Promise<void>;

/** 将真实 Koishi Session 归一化后直接交给 IM Application 的同进程插件。 */
export class VoiceLifeKoishiPlugin {
    private disposers: (() => boolean)[] = [];

    /**
     * 创建 VoiceLife Koishi 插件。
     * @param context 真实 Koishi Context。
     * @param capability 当前平台的能力与入站归一化适配器。
     * @param platformEvents 平台无关的 Application 入口。
     * @param onError 可选的受控错误上报器；默认写入 Koishi 日志。
     */
    public constructor(
        private readonly context: Context,
        private readonly capability: PlatformCapabilityPort,
        private readonly platformEvents: PlatformEventApplication,
        private readonly onError: VoiceLifeKoishiPluginErrorHandler = (error, session) =>
            context
                .logger('voicelife-im-gateway')
                .error('Koishi inbound handler failed for platform %s: %o', session.platform, error),
    ) {}

    /** 启动插件并监听消息与按钮交互事件。 */
    public start(): void {
        if (this.disposers.length > 0) return;
        const handle = async (session: Session): Promise<void> => {
            if (session.platform !== this.capability.platform) return;
            try {
                const event = await this.capability.normalizeInbound(session);
                await this.platformEvents.postEvent(event);
            } catch (error) {
                try {
                    await this.onError(error, session);
                } catch {
                    // Error reporting must not reintroduce an unhandled async listener rejection.
                }
            }
        };
        this.disposers = [this.context.on('message', handle), this.context.on('interaction/button', handle)];
    }

    /** 停止插件并注销全部 Koishi 事件监听器。 */
    public stop(): void {
        for (const dispose of this.disposers.splice(0)) dispose();
    }
}
