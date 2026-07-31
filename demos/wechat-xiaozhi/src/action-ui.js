export const REMINDER_ACTION_ROUTE = "/voicelife/reminder-actions";

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

export function reminderActionPath(token) {
  return `${REMINDER_ACTION_ROUTE}/${encodeURIComponent(token)}`;
}

export function formatChinaTime(value) {
  return new Intl.DateTimeFormat("zh-CN", {
    timeZone: "Asia/Shanghai",
    month: "long",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    hour12: false
  }).format(new Date(value));
}

export function actionPage({ reminder, token }) {
  const action = escapeHtml(reminderActionPath(token));
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
      <form method="post" action="${action}">
        <input type="hidden" name="action" value="dismiss">
        <button class="primary" type="submit">知道了</button>
      </form>
      <form method="post" action="${action}">
        <input type="hidden" name="action" value="snooze">
        <button class="secondary" type="submit">推迟 10 分钟</button>
      </form>
    </div>
  </section>
  <p class="hint">操作后可直接关闭此页面</p>
</main>
</html>`;
}

export function resultPage({ title, detail }) {
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

export function errorPage(message) {
  return `<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>无法处理提醒</title><style>
body{margin:0;background:#f4f6f3;color:#17201b;font-family:system-ui,-apple-system,"PingFang SC",sans-serif}
main{width:min(100% - 40px,440px);margin:0 auto;padding:84px 0;text-align:center}
h1{font-size:25px}p{color:#68736d}
</style><main><h1>无法处理提醒</h1><p>${escapeHtml(message)}</p></main></html>`;
}
