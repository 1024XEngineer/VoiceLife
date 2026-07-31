import { createReminderActionToken } from "./action-token.js";

export class WechatApi {
  constructor(config, { fetchImpl = globalThis.fetch, now = () => Date.now() } = {}) {
    this.config = config;
    this.fetch = fetchImpl;
    this.now = now;
    this.tokenCache = null;
  }

  assertLiveConfig() {
    const missing = [];
    if (!this.config.appId) missing.push("WECHAT_APP_ID");
    if (!this.config.appSecret) missing.push("WECHAT_APP_SECRET");
    if (missing.length) throw new Error(`微信 live 模式缺少配置：${missing.join(", ")}`);
  }

  async accessToken() {
    this.assertLiveConfig();
    if (this.tokenCache && this.tokenCache.expiresAt > this.now() + 60_000) {
      return this.tokenCache.value;
    }
    const url = new URL("https://api.weixin.qq.com/cgi-bin/token");
    url.searchParams.set("grant_type", "client_credential");
    url.searchParams.set("appid", this.config.appId);
    url.searchParams.set("secret", this.config.appSecret);
    const response = await this.fetch(url);
    const body = await response.json();
    if (!response.ok || body.errcode) {
      throw new Error(`获取微信 access_token 失败：${body.errcode ?? response.status} ${body.errmsg ?? ""}`);
    }
    this.tokenCache = {
      value: body.access_token,
      expiresAt: this.now() + Number(body.expires_in ?? 7200) * 1000
    };
    return this.tokenCache.value;
  }

  async sendTemplate({ openId, reminder }) {
    if (this.config.outboundMode !== "live") {
      return {
        errcode: 0,
        errmsg: "mock",
        msgid: `mock-${reminder.id}-${this.now()}`
      };
    }
    if (!this.config.templateId) {
      throw new Error("微信 live 模式发送提醒需要 WECHAT_TEMPLATE_ID");
    }
    const token = await this.accessToken();
    const url = new URL("https://api.weixin.qq.com/cgi-bin/message/template/send");
    url.searchParams.set("access_token", token);
    const fields = this.config.templateFields;
    const body = {
      touser: openId,
      template_id: this.config.templateId,
      data: {
        [fields.title]: { value: reminder.title },
        [fields.time]: { value: reminder.dueAt },
        [fields.status]: { value: "待处理" }
      }
    };
    if (this.config.detailUrl) {
      const detail = new URL(this.config.detailUrl);
      const token = createReminderActionToken({
        reminder,
        secret: this.config.actionTokenSecret,
        now: this.now(),
        ttlSeconds: this.config.actionTokenTtlSeconds
      });
      detail.pathname = `${detail.pathname.replace(/\/+$/, "")}/${encodeURIComponent(token)}`;
      body.url = detail.toString();
    }
    const response = await this.fetch(url, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body)
    });
    const raw = await response.text();
    const result = JSON.parse(raw);
    // 微信 msgid 已超过 JavaScript Number.MAX_SAFE_INTEGER。先从原始 JSON
    // 中提取十进制字符串，避免发送记录与 TEMPLATESENDJOBFINISH 回执无法匹配。
    const exactMessageId = raw.match(/"msgid"\s*:\s*(?:"([^"]+)"|(\d+))/);
    if (exactMessageId) result.msgid = exactMessageId[1] || exactMessageId[2];
    if (!response.ok || result.errcode) {
      throw new Error(`发送微信模板消息失败：${result.errcode ?? response.status} ${result.errmsg ?? ""}`);
    }
    return result;
  }

  async createBindingQr({ scene, expiresInSeconds = 600 }) {
    if (this.config.outboundMode !== "live") {
      return {
        ticket: `mock-ticket-${scene}`,
        expire_seconds: expiresInSeconds,
        url: `https://example.invalid/bind/${encodeURIComponent(scene)}`,
        imageUrl: `https://mp.weixin.qq.com/cgi-bin/showqrcode?ticket=${encodeURIComponent(`mock-ticket-${scene}`)}`
      };
    }
    const token = await this.accessToken();
    const url = new URL("https://api.weixin.qq.com/cgi-bin/qrcode/create");
    url.searchParams.set("access_token", token);
    const response = await this.fetch(url, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        expire_seconds: expiresInSeconds,
        action_name: "QR_STR_SCENE",
        action_info: { scene: { scene_str: scene } }
      })
    });
    const result = await response.json();
    if (!response.ok || result.errcode) {
      throw new Error(`创建微信参数二维码失败：${result.errcode ?? response.status} ${result.errmsg ?? ""}`);
    }
    return {
      ...result,
      imageUrl: `https://mp.weixin.qq.com/cgi-bin/showqrcode?ticket=${encodeURIComponent(result.ticket)}`
    };
  }
}
