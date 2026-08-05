import type { ImChannelPort, ImSendAcceptance, OutboundImMessage } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { JsonValue } from '../../shared/types.js';

/** Koishi Bot 的最小抽象，避免其类型泄漏到应用层。 */
export interface KoishiBotFacade {
    /**
     * 通过指定 Koishi Bot 发送私聊消息。
     * @param input Bot、平台用户和消息载荷。
     * @returns 平台生成的消息标识。
     */
    sendPrivateMessage(input: {
        readonly koishiBotId: string;
        readonly platformUserId: string;
        readonly content: JsonValue;
    }): Promise<{ readonly platformMessageId: string }>;
}

/** 通过 Koishi Bot 向外部 IM 平台发送消息的适配器。 */
export class KoishiChannelAdapter implements ImChannelPort {
    /** @param bot Koishi Bot 最小能力抽象。 */
    public constructor(private readonly bot: KoishiBotFacade) {}

    /** {@inheritDoc ImChannelPort.send} */
    public async send(message: OutboundImMessage): Promise<ImSendAcceptance> {
        // TODO: resolve koishiBotId through ChannelAccountRepository and delegate to Bot.
        void message;
        void this.bot;
        throw new ImGatewayError('not_implemented', 'Koishi send adapter is an empty architecture skeleton');
    }
}
