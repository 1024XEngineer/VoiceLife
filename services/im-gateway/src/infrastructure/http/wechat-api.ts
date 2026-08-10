import type { PlatformEventApplication } from '../../application/api.js';
import type { NormalizedImEvent } from '../../contracts/platform-events.js';
import type { WechatWebhookRequest, WechatOfficialAdapter } from '../wechat/wechat-official-adapter.js';

const HELP_MESSAGE = '欢迎使用 VoiceLife。\n发送绑定码：绑定 ABC123\n输入“帮助”可再次查看说明。';

/** 微信 Webhook POST 的响应正文与媒体类型。 */
export interface WechatWebhookPostResponse {
    readonly body: string;
    readonly contentType: 'application/xml; charset=utf-8' | 'text/plain; charset=utf-8';
}

/** 微信公众号 Webhook 的框架无关控制器。 */
export class WechatWebhookController {
    /**
     * 创建微信公众号 Webhook 控制器。
     * @param adapter 微信公众号验签与归一化适配器。
     * @param platformEvents 规范化事件应用入口。
     */
    public constructor(
        private readonly adapter: WechatOfficialAdapter,
        private readonly platformEvents: PlatformEventApplication,
    ) {}

    /**
     * 处理微信后台服务器配置校验请求。
     * @param request 已由 HTTP 框架映射的微信 query 参数。
     * @returns 验证成功时的 echostr。
     */
    public verify(request: WechatWebhookRequest): string | undefined {
        return this.adapter.verifyWebhook(request);
    }

    /**
     * 处理微信公众号 POST webhook，并提交规范化事件。
     * @param request 已由 HTTP 框架映射的微信请求。
     * @returns 微信要求的成功文本，或针对帮助及未知文本的被动 XML 回复。
     */
    public async post(request: WechatWebhookRequest): Promise<WechatWebhookPostResponse> {
        const event = await this.adapter.normalizeInbound(request);
        await this.platformEvents.postEvent(event);
        if (isTextMessage(event)) {
            return {
                body: this.adapter.renderPassiveTextReply(event.payload.externalUserId, HELP_MESSAGE),
                contentType: 'application/xml; charset=utf-8',
            };
        }
        return { body: 'success', contentType: 'text/plain; charset=utf-8' };
    }
}

function isTextMessage(event: NormalizedImEvent): event is Extract<
    NormalizedImEvent,
    { readonly type: 'message.received' }
> & {
    readonly payload: { readonly externalUserId: string; readonly text: string };
} {
    if (event.type !== 'message.received' || typeof event.payload !== 'object' || event.payload === null) return false;
    const payload = event.payload as Record<string, unknown>;
    return typeof payload.externalUserId === 'string' && typeof payload.text === 'string';
}
