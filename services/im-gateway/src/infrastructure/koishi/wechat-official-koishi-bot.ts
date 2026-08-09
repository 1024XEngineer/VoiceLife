import { Bot, type Context, type Fragment } from '@koishijs/core';

import type { ImSendAcceptance } from '../../ports/external.js';
import type { JsonValue } from '../../shared/types.js';
import { KoishiPlatformSendError } from './koishi-channel-adapter.js';

/** Koishi 微信 Bot 使用的已渲染消息传输端口。 */
export interface WechatOfficialKoishiTransport {
    /**
     * @param externalUserId 微信 OpenID，仅驻留当前发送调用。
     * @param content 已由 DeliveryRenderer 生成的微信模板载荷。
     * @returns 微信平台的受理或失败分类。
     */
    sendToUser(externalUserId: string, content: JsonValue): Promise<ImSendAcceptance>;
}

/** 注册生产微信公众号 Koishi Bot 所需的账号与传输配置。 */
export interface WechatOfficialKoishiBotOptions {
    readonly koishiBotId: string;
    readonly selfId: string;
    readonly transport: WechatOfficialKoishiTransport;
}

/** 将微信公众号 REST 传输注册为真实 Koishi Context Bot。 */
export class WechatOfficialKoishiBot extends Bot<Context, WechatOfficialKoishiBotOptions> {
    private readonly configuredSid: string;

    /** @param context 生产 Koishi Context。 @param options Bot 标识与微信传输端口。 */
    public constructor(context: Context, options: WechatOfficialKoishiBotOptions) {
        super(context, options, 'wechat_official');
        if (options.koishiBotId.trim() === '' || options.selfId.trim() === '') {
            throw new Error('WeChat Koishi bot identifiers are required');
        }
        this.configuredSid = options.koishiBotId.trim();
        this.platform = 'wechat_official';
        this.user = { id: options.selfId.trim() };
    }

    /** @returns ChannelAccount 持久化并由 Koishi facade 查询的稳定 Bot 标识。 */
    public override get sid(): string {
        return this.configuredSid;
    }

    /** @returns Bot 进入在线状态后兑现的 Promise。 */
    public override start(): Promise<void> {
        this.online();
        return Promise.resolve();
    }

    /** @returns Bot 离线后兑现的 Promise。 */
    public override stop(): Promise<void> {
        this.offline();
        return Promise.resolve();
    }

    /** @returns Bot 从活动 Context 注销或随 Context 安全停止后兑现的 Promise。 */
    public override dispose(): Promise<void> {
        return this.ctx.bots === undefined ? this.stop() : super.dispose();
    }

    /**
     * @param userId 目标微信 OpenID。
     * @param content Koishi Channel Adapter 传入的已渲染载荷。
     * @returns 微信平台消息 ID 列表。
     */
    public override async sendPrivateMessage(userId: string, content: Fragment): Promise<string[]> {
        const acceptance = await this.config.transport.sendToUser(userId, renderedContent(content));
        if (!acceptance.accepted) {
            throw new KoishiPlatformSendError(
                acceptance.retryable === true,
                acceptance.errorCode ?? 'wechat_send_rejected',
            );
        }
        if (acceptance.platformMessageId === undefined || acceptance.platformMessageId.trim() === '') {
            throw new KoishiPlatformSendError(true, 'wechat_missing_message_id');
        }
        return [acceptance.platformMessageId];
    }
}

function renderedContent(content: Fragment): JsonValue {
    if (typeof content !== 'string') {
        throw new KoishiPlatformSendError(false, 'koishi_unsupported_fragment');
    }
    try {
        const parsed: unknown = JSON.parse(content);
        if (isJsonValue(parsed)) return parsed;
    } catch {
        // Plain strings remain valid JSON scalar content.
    }
    return content;
}

function isJsonValue(value: unknown): value is JsonValue {
    if (value === null || ['string', 'number', 'boolean'].includes(typeof value)) return true;
    if (Array.isArray(value)) return value.every(isJsonValue);
    if (typeof value !== 'object') return false;
    return Object.values(value).every(isJsonValue);
}
