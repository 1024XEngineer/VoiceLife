import {
  actionPage,
  errorPage,
  formatChinaTime,
  resultPage
} from "./app.js";
import { verifyReminderActionToken } from "./action-token.js";
import {
  processWechatCallback,
  verifyWechatUrl
} from "./wechat-handler.js";
import {
  deactivateWechatBinding,
  handleWechatBindingEvent,
  handleWechatText,
  handleWechatVoice
} from "./wechat-domain.js";
import { parseWechatXml } from "./xml.js";

function html(koa, status, body) {
  koa.status = status;
  koa.type = "text/html; charset=utf-8";
  koa.body = body;
}

function json(koa, status, body) {
  koa.status = status;
  koa.type = "application/json; charset=utf-8";
  koa.body = body;
}

function authorized(koa, apiKey) {
  return koa.headers.authorization === `Bearer ${apiKey}`;
}

function requestUrl(koa, baseUrl) {
  return new URL(koa.request.url, baseUrl);
}

function formBody(body) {
  if (body && typeof body === "object") return body;
  return Object.fromEntries(new URLSearchParams(String(body || "")));
}

function logWechatAccount(logger, body) {
  const message = parseWechatXml(body);
  if (message.ToUserName) {
    logger.info(`[wechat] 公众号原始 ID：${message.ToUserName}`);
  }
  return message;
}

export function createVoiceLifeKoishiPlugin({ config, service, wechatApi, logger = console }) {
  function voiceLifeKoishiPlugin(ctx) {
    const koishiLogger = ctx.logger("voicelife");

    ctx.middleware(async (session, next) => {
      if (session.platform !== "wechat-official") return next();
      const raw = session.wechatOfficial;
      const reply = raw?.MsgType === "voice"
        ? handleWechatVoice({
            recognition: raw.Recognition,
            mediaId: raw.MediaId
          })
        : handleWechatText({
            content: session.content,
            openId: session.userId,
            service
          });
      koishiLogger.info("wechat message user=%s", session.userId);
      await session.send(reply);
    });

    ctx.on("friend-added", async (session) => {
      if (session.platform !== "wechat-official") return;
      const reply = handleWechatBindingEvent({
        eventKey: session.wechatOfficial?.EventKey,
        openId: session.userId,
        service
      });
      await session.send(reply);
    });

    ctx.on("friend-deleted", (session) => {
      if (session.platform !== "wechat-official") return;
      deactivateWechatBinding({ openId: session.userId, service });
    });

    // 官方适配器当前的明文 POST 不校验 signature，安全模式 URL 验证也不完整。
    // 在适配器路由之前使用现有实现完成统一 URL 验证和明文消息验签。
    ctx.server.get("/wechat-official", async (koa) => {
      try {
        const echo = await verifyWechatUrl(
          requestUrl(koa, config.koishi.selfUrl),
          config.wechat
        );
        koa.status = 200;
        koa.body = echo;
      } catch (error) {
        koa.status = 403;
        koa.body = "invalid signature";
      }
    });

    // 官方适配器当前没有标准化 SCAN 和 TEMPLATESENDJOBFINISH。
    // 在进入适配器前补齐这两个微信专属事件，普通消息验签后仍由 Koishi 处理。
    ctx.server.post("/wechat-official", async (koa, next) => {
      const body = typeof koa.request.body === "string"
        ? koa.request.body
        : String(koa.request.body || "");
      const message = parseWechatXml(body);
      if (!message.Encrypt) {
        try {
          await verifyWechatUrl(
            requestUrl(koa, config.koishi.selfUrl),
            config.wechat
          );
        } catch {
          koa.status = 403;
          koa.body = "invalid signature";
          return;
        }
      }
      const event = message.Event.toLowerCase();
      if (
        message.MsgType.toLowerCase() !== "event" ||
        !["scan", "templatesendjobfinish"].includes(event)
      ) {
        return next();
      }
      const reply = await processWechatCallback({
        url: requestUrl(koa, config.koishi.selfUrl),
        body,
        config: config.wechat,
        service
      });
      koa.status = 200;
      koa.type = "application/xml; charset=utf-8";
      koa.body = reply || "success";
    });

    // 迁移兼容入口：公众号后台切到 /wechat-official 前，旧 URL 仍可工作，
    // 同时会在日志中打印 Koishi 必需的公众号原始 ID。
    ctx.server.get("/wechat/callback", async (koa) => {
      const echo = await verifyWechatUrl(
        requestUrl(koa, config.koishi.selfUrl),
        config.wechat
      );
      koa.status = 200;
      koa.body = echo;
    });
    ctx.server.post("/wechat/callback", async (koa) => {
      const body = typeof koa.request.body === "string"
        ? koa.request.body
        : String(koa.request.body || "");
      logWechatAccount(logger, body);
      const reply = await processWechatCallback({
        url: requestUrl(koa, config.koishi.selfUrl),
        body,
        config: config.wechat,
        service
      });
      koa.status = 200;
      koa.type = "application/xml; charset=utf-8";
      koa.body = reply || "success";
    });

    ctx.server.get("/", (koa) => {
      html(koa, 200, `<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>VoiceLife Koishi + Satori Demo</title>
<style>body{font:16px/1.6 system-ui,sans-serif;max-width:760px;margin:48px auto;padding:0 20px;color:#18201d}code{background:#edf2ef;padding:2px 6px;border-radius:5px}li{margin:8px 0}</style>
<h1>VoiceLife Koishi + Satori Demo</h1>
<ul>
<li>Koishi 微信回调：<code>/wechat-official</code></li>
<li>旧回调兼容：<code>/wechat/callback</code></li>
<li>Satori API：<code>${config.koishi.satoriPath}/v1</code></li>
<li>提醒操作页：<code>/reminders/action</code></li>
</ul></html>`);
    });

    ctx.server.get("/health", (koa) => {
      json(koa, 200, {
        ok: true,
        runtime: config.runtime,
        wechatMode: config.wechat.outboundMode,
        wechatConfigured: Boolean(config.wechat.appId && config.wechat.appSecret),
        wechatAccountConfigured: Boolean(config.wechat.account),
        koishiBots: ctx.bots.map((bot) => ({
          platform: bot.platform,
          selfId: bot.selfId,
          status: bot.status
        })),
        satoriPath: config.koishi.satoriPath,
        xiaozhiConfigured: Boolean(config.xiaozhi.endpoint)
      });
    });

    ctx.server.get("/reminders/action", (koa) => {
      try {
        const token = koa.query.token;
        const payload = verifyReminderActionToken(
          token,
          config.wechat.actionTokenSecret
        );
        const reminder = service.findWebActionTarget(payload);
        html(koa, 200, actionPage({ reminder, token }));
      } catch (error) {
        html(koa, 400, errorPage(error.message));
      }
    });

    ctx.server.post("/reminders/action", (koa) => {
      try {
        const form = formBody(koa.request.body);
        const payload = verifyReminderActionToken(
          form.token,
          config.wechat.actionTokenSecret
        );
        if (form.action === "dismiss") {
          service.dismissFromWeb(payload);
          return html(
            koa,
            200,
            resultPage({ title: "已知道", detail: "这条提醒已经完成" })
          );
        }
        if (form.action === "snooze") {
          const reminder = service.snoozeFromWeb(payload, 10);
          return html(
            koa,
            200,
            resultPage({
              title: "已推迟 10 分钟",
              detail: `将在 ${formatChinaTime(reminder.dueAt)} 再次提醒`
            })
          );
        }
        throw new Error("未知操作");
      } catch (error) {
        html(koa, 400, errorPage(error.message));
      }
    });

    ctx.server.use(async (koa, next) => {
      if (!koa.path.startsWith("/api/")) return next();
      if (authorized(koa, config.deviceApiKey)) return next();
      json(koa, 401, { error: "unauthorized" });
    });

    ctx.server.get("/api/state", (koa) => {
      json(koa, 200, service.store.snapshot());
    });
    ctx.server.post("/api/binding-codes", async (koa) => {
      const input = koa.request.body || {};
      const binding = service.createBindingCode(input.deviceId);
      const qr = input.withQr
        ? await wechatApi.createBindingQr({ scene: binding.code })
        : null;
      json(koa, 201, { ...binding, qr });
    });
    ctx.server.post("/api/demo/bind", (koa) => {
      if (!config.demoMode) return json(koa, 404, { error: "not_found" });
      const input = koa.request.body || {};
      json(koa, 200, service.bindOpenId(input.code, input.openId));
    });
    ctx.server.post("/api/reminders", (koa) => {
      json(koa, 201, service.createReminder(koa.request.body || {}));
    });
    ctx.server.post(/^\/api\/reminders\/([^/]+)\/dispatch$/, async (koa) => {
      json(koa, 200, await service.dispatchReminder(koa.params[0]));
    });
  }
  voiceLifeKoishiPlugin.inject = ["server"];
  return voiceLifeKoishiPlugin;
}
