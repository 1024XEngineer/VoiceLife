import { type Context, type Fragment } from '@koishijs/core';

import type { ImChannelPort, ImSendAcceptance, OutboundImMessage } from '../../ports/external.js';
import type { ImUnitOfWork } from '../../ports/repositories.js';
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

/** Koishi 渠道发送适配器所需的账号、身份与 Bot 依赖。 */
export interface KoishiChannelAdapterOptions {
    readonly unitOfWork: ImUnitOfWork;
    readonly bot: KoishiBotFacade;
    /**
     * 解密仅供当前发送使用的外部用户标识。
     * @param ciphertext 持久化的受保护外部用户标识。
     * @returns Koishi Bot 使用的平台用户标识。
     */
    revealExternalUserId(ciphertext: string): Promise<string>;
}

/** 通过 Koishi Bot 向外部 IM 平台发送消息的适配器。 */
export class KoishiChannelAdapter implements ImChannelPort {
    /** @param options 账号仓储、身份解密器与 Koishi Bot facade。 */
    public constructor(private readonly options: KoishiChannelAdapterOptions) {}

    /** {@inheritDoc ImChannelPort.send} */
    public async send(message: OutboundImMessage): Promise<ImSendAcceptance> {
        if (
            message.conversation.kind !== 'direct' ||
            message.conversation.channelAccountId !== message.delivery.channelAccountId
        ) {
            return message.conversation.kind !== 'direct'
                ? { accepted: false, retryable: false, errorCode: 'koishi_direct_message_required' }
                : { accepted: false, retryable: false, errorCode: 'koishi_conversation_mismatch' };
        }
        const account = await this.options.unitOfWork.transaction((context) =>
            context.channelAccounts.findById(message.delivery.channelAccountId),
        );
        if (account === undefined || account.status !== 'active' || account.koishiBotId.trim() === '') {
            return { accepted: false, retryable: false, errorCode: 'koishi_channel_account_unavailable' };
        }
        const platformUserId = (
            await this.options.revealExternalUserId(message.conversation.externalConversationIdCiphertext)
        ).trim();
        if (platformUserId === '') {
            return { accepted: false, retryable: false, errorCode: 'koishi_invalid_recipient' };
        }
        try {
            const result = await this.options.bot.sendPrivateMessage({
                koishiBotId: account.koishiBotId,
                platformUserId,
                content: message.content,
            });
            const platformMessageId = result.platformMessageId.trim();
            return platformMessageId === ''
                ? { accepted: false, retryable: true, errorCode: 'koishi_missing_message_id' }
                : { accepted: true, platformMessageId };
        } catch (error) {
            if (error instanceof KoishiBotUnavailableError) {
                return { accepted: false, retryable: true, errorCode: 'koishi_bot_unavailable' };
            }
            throw error;
        }
    }
}

/** 使用真实 Koishi Context 查找 Bot 并发送私聊消息的 facade。 */
export class KoishiContextBotFacade implements KoishiBotFacade {
    /** @param context 当前进程内的真实 Koishi Context。 */
    public constructor(private readonly context: Context) {}

    /** {@inheritDoc KoishiBotFacade.sendPrivateMessage} */
    public async sendPrivateMessage(input: {
        readonly koishiBotId: string;
        readonly platformUserId: string;
        readonly content: JsonValue;
    }): Promise<{ readonly platformMessageId: string }> {
        const bot =
            this.context.bots[input.koishiBotId] ??
            this.context.bots.find((candidate) => candidate.sid === input.koishiBotId);
        if (bot === undefined || !bot.isActive) throw new KoishiBotUnavailableError();
        const messageIds = await bot.sendPrivateMessage(input.platformUserId, toKoishiFragment(input.content));
        return { platformMessageId: messageIds[0] ?? '' };
    }
}

class KoishiBotUnavailableError extends Error {
    public constructor() {
        super('The configured Koishi Bot is unavailable');
        this.name = 'KoishiBotUnavailableError';
    }
}

function toKoishiFragment(content: JsonValue): Fragment {
    if (typeof content === 'string') return content;
    if (
        typeof content === 'object' &&
        content !== null &&
        !Array.isArray(content) &&
        content.type === 'text' &&
        typeof content.text === 'string'
    ) {
        return content.text;
    }
    return JSON.stringify(content);
}
