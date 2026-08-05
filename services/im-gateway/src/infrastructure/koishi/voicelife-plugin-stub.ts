import type { PlatformEventApplication } from '../../application/api.js';
import type { PlatformCapabilityPort } from '../../ports/external.js';

/** Structural seam for the real Koishi Context. */
export interface KoishiPluginContextFacade {
    onPlatformEvent(listener: (rawEvent: unknown) => Promise<void>): () => void;
}

/**
 * Koishi plugin lifecycle skeleton. Real code may import Koishi only in this
 * infrastructure package and must pass normalized events inward.
 */
export class VoiceLifeKoishiPluginStub {
    private dispose: (() => void) | undefined;

    public constructor(
        private readonly context: KoishiPluginContextFacade,
        private readonly capability: PlatformCapabilityPort,
        private readonly platformEvents: PlatformEventApplication,
    ) {}

    public start(): void {
        this.dispose = this.context.onPlatformEvent(async (rawEvent) => {
            const event = await this.capability.normalizeInbound(rawEvent);
            await this.platformEvents.postEvent(event);
        });
    }

    public stop(): void {
        this.dispose?.();
        this.dispose = undefined;
    }
}
