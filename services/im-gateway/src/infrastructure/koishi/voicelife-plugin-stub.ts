import type { PlatformEventApplication } from '../../application/api.js';
import type { PlatformCapabilityPort } from '../../ports/external.js';

/** 真实 Koishi Context 的最小结构抽象。 */
export interface KoishiPluginContextFacade {
    /**
     * 注册平台原始事件监听器。
     * @param listener 接收未归一化事件的异步回调。
     * @returns 用于注销监听器的函数。
     */
    onPlatformEvent(listener: (rawEvent: unknown) => Promise<void>): () => void;
}

/**
 * Koishi 插件生命周期骨架。
 * 真实实现只能在基础设施层导入 Koishi，并向内传递规范化事件。
 */
export class VoiceLifeKoishiPluginStub {
    private dispose: (() => void) | undefined;

    /**
     * 创建 Koishi 插件生命周期实例。
     * @param context Koishi Context 最小抽象。
     * @param capability 平台能力适配器。
     * @param platformEvents 规范化事件应用入口。
     */
    public constructor(
        private readonly context: KoishiPluginContextFacade,
        private readonly capability: PlatformCapabilityPort,
        private readonly platformEvents: PlatformEventApplication,
    ) {}

    /** 启动插件并注册平台事件监听器。 */
    public start(): void {
        this.dispose = this.context.onPlatformEvent(async (rawEvent) => {
            const event = await this.capability.normalizeInbound(rawEvent);
            await this.platformEvents.postEvent(event);
        });
    }

    /** 停止插件并注销平台事件监听器。 */
    public stop(): void {
        this.dispose?.();
        this.dispose = undefined;
    }
}
