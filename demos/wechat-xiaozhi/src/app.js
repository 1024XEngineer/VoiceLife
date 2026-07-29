import http from "node:http";
import { URL } from "node:url";
import { verifyReminderActionToken } from "./action-token.js";
import { processWechatCallback, verifyWechatUrl } from "./wechat-handler.js";

function json(response, status, body) {
  response.writeHead(status, { "content-type": "application/json; charset=utf-8" });
  response.end(`${JSON.stringify(body, null, 2)}\n`);
}

function text(response, status, body, contentType = "text/plain; charset=utf-8") {
  response.writeHead(status, { "content-type": contentType });
  response.end(body);
}

async function readBody(request, limit = 1024 * 1024) {
  const chunks = [];
  let size = 0;
  for await (const chunk of request) {
    size += chunk.length;
    if (size > limit) throw new Error("请求体超过 1 MiB");
    chunks.push(chunk);
  }
  return Buffer.concat(chunks).toString();
}

function authorized(request, apiKey) {
  return request.headers.authorization === `Bearer ${apiKey}`;
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function formatChinaTime(value) {
  return new Intl.DateTimeFormat("zh-CN", {
    timeZone: "Asia/Shanghai",
    month: "long",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    hour12: false
  }).format(new Date(value));
}

function actionPage({ reminder, token }) {
  return `<!doctype html>
<html lang="zh-CN">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#f4f6f3">
<title>处理提醒</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#f4f6f3;color:#17201b;font-family:system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}
main{width:min(100% - 32px,480px);margin:0 auto;padding:48px 0 max(28px,env(safe-area-inset-bottom))}
.eyebrow{margin:0 0 14px;color:#64716a;font-size:14px;letter-spacing:.08em}
.card{background:#fff;border:1px solid #e2e7e3;border-radius:24px;padding:28px 24px;box-shadow:0 14px 40px rgba(26,43,34,.08)}
h1{margin:0 0 10px;font-size:28px;line-height:1.3;overflow-wrap:anywhere}
.time{margin:0;color:#66716b;font-size:16px}
.actions{display:grid;gap:12px;margin-top:28px}
form{margin:0}
button{width:100%;min-height:54px;border:0;border-radius:14px;font:600 17px system-ui,-apple-system,"PingFang SC",sans-serif;cursor:pointer}
.primary{background:#16794b;color:#fff}
.secondary{background:#e9f2ed;color:#11633d}
.hint{margin:18px 4px 0;text-align:center;color:#7a847e;font-size:13px}
</style>
<main>
  <p class="eyebrow">VOICELIFE · 提醒</p>
  <section class="card">
    <h1>${escapeHtml(reminder.title)}</h1>
    <p class="time">${escapeHtml(formatChinaTime(reminder.dueAt))}</p>
    <div class="actions">
      <form method="post" action="/reminders/action">
        <input type="hidden" name="token" value="${escapeHtml(token)}">
        <input type="hidden" name="action" value="dismiss">
        <button class="primary" type="submit">知道了</button>
      </form>
      <form method="post" action="/reminders/action">
        <input type="hidden" name="token" value="${escapeHtml(token)}">
        <input type="hidden" name="action" value="snooze">
        <button class="secondary" type="submit">推迟 10 分钟</button>
      </form>
    </div>
  </section>
  <p class="hint">操作后可直接关闭此页面</p>
</main>
</html>`;
}

function resultPage({ title, detail }) {
  return `<!doctype html>
<html lang="zh-CN">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#16794b">
<title>${escapeHtml(title)}</title>
<style>
body{margin:0;background:#f4f6f3;color:#17201b;font-family:system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}
main{width:min(100% - 40px,440px);margin:0 auto;padding:84px 0;text-align:center}
.icon{display:grid;place-items:center;width:72px;height:72px;margin:0 auto 22px;border-radius:50%;background:#16794b;color:#fff;font-size:36px}
h1{margin:0 0 10px;font-size:27px}p{margin:0;color:#68736d;font-size:16px}
</style>
<main><div class="icon">✓</div><h1>${escapeHtml(title)}</h1><p>${escapeHtml(detail)}</p></main>
</html>`;
}

function errorPage(message) {
  return `<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>无法处理提醒</title><style>
body{margin:0;background:#f4f6f3;color:#17201b;font-family:system-ui,-apple-system,"PingFang SC",sans-serif}
main{width:min(100% - 40px,440px);margin:0 auto;padding:84px 0;text-align:center}
h1{font-size:25px}p{color:#68736d}
</style><main><h1>无法处理提醒</h1><p>${escapeHtml(message)}</p></main></html>`;
}

function page(config) {
  const mode = config.wechat.outboundMode;
  return `<!doctype html>
<html lang="zh-CN">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VoiceLife 微信 × 小智 Demo</title>
<style>
body{font:16px/1.6 system-ui,sans-serif;max-width:760px;margin:48px auto;padding:0 20px;color:#18201d}
code{background:#edf2ef;padding:2px 6px;border-radius:5px}li{margin:8px 0}
.ok{color:#08783e}.warn{color:#9a5b00}
</style>
<h1>VoiceLife 微信 × 小智 Demo</h1>
<p class="${mode === "live" ? "ok" : "warn"}">微信外发模式：<strong>${mode}</strong></p>
<ol>
<li>健康检查：<a href="/health"><code>GET /health</code></a></li>
<li>公众号回调：<code>${config.baseUrl}/wechat/callback</code></li>
<li>小智 MCP：${config.xiaozhi.endpoint ? "已配置" : "未配置 XIAOZHI_MCP_ENDPOINT"}</li>
<li>带 Bearer Token 查看状态：<code>GET /api/state</code></li>
</ol>
<p>完整联调步骤请看项目 README。</p>
</html>`;
}

export function createApp({ config, service, wechatApi, logger = console }) {
  return http.createServer(async (request, response) => {
    const url = new URL(request.url, config.baseUrl);
    try {
      if (request.method === "GET" && url.pathname === "/") {
        return text(response, 200, page(config), "text/html; charset=utf-8");
      }
      if (request.method === "GET" && url.pathname === "/health") {
        return json(response, 200, {
          ok: true,
          wechatMode: config.wechat.outboundMode,
          wechatConfigured: Boolean(config.wechat.appId && config.wechat.appSecret),
          wechatAesConfigured: Boolean(config.wechat.aesKey),
          xiaozhiConfigured: Boolean(config.xiaozhi.endpoint)
        });
      }
      if (request.method === "GET" && url.pathname === "/wechat/callback") {
        logger.info("[wechat] 收到服务器 URL 校验请求");
        const echo = await verifyWechatUrl(url, config.wechat);
        logger.info("[wechat] 服务器 URL 校验成功");
        return text(response, 200, echo);
      }
      if (request.method === "POST" && url.pathname === "/wechat/callback") {
        const body = await readBody(request);
        logger.info(
          `[wechat] 收到消息回调：mode=${body.includes("<Encrypt>") ? "aes" : "plain"} bytes=${Buffer.byteLength(body)}`
        );
        const reply = await processWechatCallback({
          url,
          body,
          config: config.wechat,
          service
        });
        logger.info(`[wechat] 回调处理成功：replyBytes=${Buffer.byteLength(reply)}`);
        return text(response, 200, reply, "application/xml; charset=utf-8");
      }
      if (request.method === "GET" && url.pathname === "/reminders/action") {
        try {
          const token = url.searchParams.get("token");
          const payload = verifyReminderActionToken(
            token,
            config.wechat.actionTokenSecret
          );
          const reminder = service.findWebActionTarget(payload);
          return text(
            response,
            200,
            actionPage({ reminder, token }),
            "text/html; charset=utf-8"
          );
        } catch (error) {
          return text(response, 400, errorPage(error.message), "text/html; charset=utf-8");
        }
      }
      if (request.method === "POST" && url.pathname === "/reminders/action") {
        try {
          const form = new URLSearchParams(await readBody(request, 16 * 1024));
          const token = form.get("token");
          const payload = verifyReminderActionToken(
            token,
            config.wechat.actionTokenSecret
          );
          if (form.get("action") === "dismiss") {
            service.dismissFromWeb(payload);
            return text(
              response,
              200,
              resultPage({ title: "已知道", detail: "这条提醒已经完成" }),
              "text/html; charset=utf-8"
            );
          }
          if (form.get("action") === "snooze") {
            const reminder = service.snoozeFromWeb(payload, 10);
            return text(
              response,
              200,
              resultPage({
                title: "已推迟 10 分钟",
                detail: `将在 ${formatChinaTime(reminder.dueAt)} 再次提醒`
              }),
              "text/html; charset=utf-8"
            );
          }
          throw new Error("未知操作");
        } catch (error) {
          return text(response, 400, errorPage(error.message), "text/html; charset=utf-8");
        }
      }

      if (url.pathname.startsWith("/api/") && !authorized(request, config.deviceApiKey)) {
        return json(response, 401, { error: "unauthorized" });
      }
      if (request.method === "GET" && url.pathname === "/api/state") {
        return json(response, 200, service.store.snapshot());
      }
      if (request.method === "POST" && url.pathname === "/api/binding-codes") {
        const input = JSON.parse((await readBody(request)) || "{}");
        const binding = service.createBindingCode(input.deviceId);
        const qr = input.withQr
          ? await wechatApi.createBindingQr({ scene: binding.code })
          : null;
        return json(response, 201, { ...binding, qr });
      }
      if (request.method === "POST" && url.pathname === "/api/demo/bind") {
        if (!config.demoMode) return json(response, 404, { error: "not_found" });
        const input = JSON.parse((await readBody(request)) || "{}");
        return json(response, 200, service.bindOpenId(input.code, input.openId));
      }
      if (request.method === "POST" && url.pathname === "/api/reminders") {
        const input = JSON.parse((await readBody(request)) || "{}");
        return json(response, 201, service.createReminder(input));
      }
      const dispatchMatch = url.pathname.match(/^\/api\/reminders\/([^/]+)\/dispatch$/);
      if (request.method === "POST" && dispatchMatch) {
        return json(response, 200, await service.dispatchReminder(dispatchMatch[1]));
      }

      return json(response, 404, { error: "not_found" });
    } catch (error) {
      logger.error(error);
      const status = /签名|unauthorized/.test(error.message) ? 403 : 400;
      return json(response, status, { error: error.message });
    }
  });
}
