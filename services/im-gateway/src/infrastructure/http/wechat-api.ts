import type { PlatformEventApplication } from '../../application/api.js';
import type { WechatWebhookRequest, WechatOfficialAdapter } from '../wechat/wechat-official-adapter.js';

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
     * @returns 微信要求的成功响应文本。
     */
    public async post(request: WechatWebhookRequest): Promise<'success'> {
        const event = await this.adapter.normalizeInbound(request);
        await this.platformEvents.postEvent(event);
        return 'success';
    }
}
