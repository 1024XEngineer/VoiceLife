import type { ImChannelPort, ImSendAcceptance, OutboundImMessage } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { JsonValue } from '../../shared/types.js';

/** Minimal seam over Koishi Context/Bot; Koishi types stay in infrastructure. */
export interface KoishiBotFacade {
    sendPrivateMessage(input: {
        readonly koishiBotId: string;
        readonly platformUserId: string;
        readonly content: JsonValue;
    }): Promise<{ readonly platformMessageId: string }>;
}

export class KoishiChannelAdapter implements ImChannelPort {
    public constructor(private readonly bot: KoishiBotFacade) {}

    public async send(message: OutboundImMessage): Promise<ImSendAcceptance> {
        // TODO: resolve koishiBotId through ChannelAccountRepository and delegate to Bot.
        void message;
        void this.bot;
        throw new ImGatewayError('not_implemented', 'Koishi send adapter is an empty architecture skeleton');
    }
}
