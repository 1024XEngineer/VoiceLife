import {
  actionPage,
  errorPage,
  formatChinaTime,
  REMINDER_ACTION_ROUTE,
  resultPage
} from "./action-ui.js";
import { createBindingHandler } from "./binding-handler.js";
import {
  createReminderActionHandler,
  registerReminderActionHandler
} from "./reminder-action.js";
import {
  processWechatCallback,
  verifyWechatUrl
} from "./wechat-handler.js";
import {
  deactivateWechatBinding,
  handleWechatBindingEvent,
  handleWechatBindingText,
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

export function createVoiceLifeKoishiPlugin({
  config,
  service,
  imApplication,
  wechatApi,
  logger = console
}) {
  const bindingHandler = createBindingHandler({
    bindingApplication: imApplication.binding
  });
  const reminderAction = createReminderActionHandler({
    actionApplication: imApplication.action,
    tokenSecret: config.wechat.actionTokenSecret
  });

  function voiceLifeKoishiPlugin(ctx) {
    const koishiLogger = ctx.logger("voicelife");

    // Koishi 标准交互入口：平台 Adapter 产生 interaction/button，
    // VoiceLife 插件只解析动作并转发给同一个提醒动作处理器。
    registerReminderActionHandler(ctx, reminderAction);

    ctx.middleware(async (session, next) => {
      if (session.platform !== "wechat-official") return next();
      const raw = session.wechatOfficial;
      const reply = raw?.MsgType === "voice"
        ? handleWechatVoice({
            recognition: raw.Recognition,
            mediaId: raw.MediaId
          })
        : handleWechatBindingText({
            channelAccountId: session.selfId || config.wechat.account || "wechat-official",
            content: session.content,
            eventId: session.messageId,
            openId: session.userId,
            bindingHandler
          });
      koishiLogger.info("wechat message user=%s", session.userId);
      await session.send(reply);
    });

    ctx.on("friend-added", async (session) => {
      if (session.platform !== "wechat-official") return;
      const reply = handleWechatBindingEvent({
        channelAccountId: session.selfId || config.wechat.account || "wechat-official",
        eventId: session.messageId || `${session.timestamp}:friend-added`,
        eventKey: session.wechatOfficial?.EventKey,
        openId: session.userId,
        bindingHandler
      });
      await session.send(reply);
    });

    ctx.on("friend-deleted", (session) => {
      if (session.platform !== "wechat-official") return;
      deactivateWechatBinding({
        channelAccountId: session.selfId || config.wechat.account || "wechat-official",
        eventId: session.messageId || `${session.timestamp}:friend-deleted`,
        openId: session.userId,
        bindingHandler
      });
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
        bindingHandler,
        receiptService: service
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
        bindingHandler,
        receiptService: service
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
<li>提醒操作页：<code>${REMINDER_ACTION_ROUTE}/{token}</code></li>
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

    ctx.server.get(`${REMINDER_ACTION_ROUTE}/:token`, (koa) => {
      try {
        const token = koa.params.token;
        const reminder = reminderAction.inspect(token);
        html(koa, 200, actionPage({ reminder, token }));
      } catch (error) {
        html(koa, 400, errorPage(error.message));
      }
    });

    ctx.server.post(`${REMINDER_ACTION_ROUTE}/:token`, (koa) => {
      try {
        const form = formBody(koa.request.body);
        const result = reminderAction.execute({
          token: koa.params.token,
          action: form.action
        });
        const detail = result.action === "snooze"
          ? `将在 ${formatChinaTime(result.reminder.dueAt)} 再次提醒`
          : result.detail;
        html(koa, 200, resultPage({ title: result.title, detail }));
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
      const result = bindingHandler.handle({
        type: "binding.requested",
        channelAccountId: input.channelAccountId || config.wechat.account || "wechat-official",
        eventId: input.eventId || `demo:${input.code}:${input.openId}`,
        actor: {
          platform: input.platform || "wechat-official",
          userId: input.openId
        },
        pairingCode: input.code
      });
      json(koa, 200, result.binding);
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
