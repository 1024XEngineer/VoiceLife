import type { ActionUiApplication, ActionUiOption, ActionUiView } from '../../application/api.js';
import type { ReminderActionCommand } from '../../contracts/device-gateway.js';
import { parseActionToken, parseReminderActionIntent } from '../../contracts/device-gateway-parser.js';
import { ImGatewayError } from '../../shared/errors.js';

/** 提醒动作页面的展示与执行路由。 */
export const ACTION_UI_ROUTES = {
    show: '/voicelife/reminder-actions/:token',
    execute: '/voicelife/reminder-actions/:token',
} as const;

/** H5 或小程序动作入口控制器，不依赖 Koishi Session。 */
export class ActionUiController {
    /** @param actionUi 动作页面应用服务。 */
    public constructor(private readonly actionUi: ActionUiApplication) {}

    /**
     * 校验路径令牌并返回动作页面视图。
     * @param token 未受信任的路径令牌。
     * @returns 可安全呈现的动作视图。
     */
    public get(token: unknown): Promise<ActionUiView> {
        return this.actionUi.show(parseActionToken(token));
    }

    /**
     * 校验并执行动作页面提交的操作。
     * @param input 未受信任的动作载荷。
     * @returns 下发给设备的动作命令。
     */
    public post(input: unknown): Promise<ReminderActionCommand> {
        return this.actionUi.execute(parseReminderActionIntent(input));
    }
}

/** H5 动作页交给 HTTP 框架写回的完整响应。 */
export interface ActionUiPageResponse {
    readonly status: 200 | 400 | 404 | 410;
    readonly headers: Readonly<Record<string, string>>;
    readonly body: string;
}

/** 接收已验证动作命令的观测端口，供生产结构化日志串联 correlationId。 */
export interface ActionUiSubmissionObserver {
    /**
     * 记录已持久化并准备下发设备的动作命令。
     * @param command 不含原始动作令牌的动作命令。
     */
    submitted(command: ReminderActionCommand): void;
}

/** 服务端渲染的移动端提醒动作页面，不向浏览器暴露内部聚合标识。 */
export class ActionUiPageController {
    /**
     * @param actionUi 动作页面应用服务。
     * @param observer 可选的脱敏动作观测端口。
     */
    public constructor(
        private readonly actionUi: ActionUiApplication,
        private readonly observer?: ActionUiSubmissionObserver,
    ) {}

    /**
     * 校验路径令牌并渲染允许的动作选项。
     * @param token 未受信任的路径令牌。
     * @returns HTML 页面响应；令牌无效或过期时返回安全终态页。
     */
    public async get(token: unknown): Promise<ActionUiPageResponse> {
        try {
            const parsedToken = parseActionToken(token);
            const view = await this.actionUi.show(parsedToken);
            return htmlResponse(
                200,
                view.state === 'available' ? renderActionPage(parsedToken, view) : renderActionStatePage(view),
            );
        } catch (error) {
            return actionUiErrorResponse(error);
        }
    }

    /**
     * 从路径取得可信令牌并执行页面提交的动作。
     * @param token 未受信任的路径令牌；请求体中的同名字段会被忽略。
     * @param input 只读取 action 与可选 params 的未受信任请求体。
     * @returns 不包含内部动作、投递或操作标识的 HTML 结果页。
     */
    public async post(token: unknown, input: unknown): Promise<ActionUiPageResponse> {
        try {
            const parsedToken = parseActionToken(token);
            const submitted = submittedAction(input);
            const intent = parseReminderActionIntent({ token: parsedToken, ...submitted });
            const command = await this.actionUi.execute(intent);
            try {
                this.observer?.submitted(command);
            } catch {
                // Observability failures must not change an already-persisted action result.
            }
            return htmlResponse(200, renderResultPage(command.action, command.params));
        } catch (error) {
            return actionUiErrorResponse(error);
        }
    }
}

const PAGE_HEADERS = {
    'content-type': 'text/html; charset=utf-8',
    'cache-control': 'no-store',
    'content-security-policy':
        "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'",
    'referrer-policy': 'no-referrer',
    'x-content-type-options': 'nosniff',
    'strict-transport-security': 'max-age=31536000; includeSubDomains',
    'permissions-policy': 'camera=(), microphone=(), geolocation=()',
} as const;

function htmlResponse(status: ActionUiPageResponse['status'], body: string): ActionUiPageResponse {
    return { status, headers: { ...PAGE_HEADERS }, body };
}

function actionUiErrorResponse(error: unknown): ActionUiPageResponse {
    if (error instanceof ImGatewayError) {
        if (error.code === 'action_expired') {
            return htmlResponse(410, renderMessagePage('链接已过期', '这条提醒的操作时间已经结束。'));
        }
        if (error.code === 'action_not_found') {
            return htmlResponse(404, renderMessagePage('链接不可用', '请从微信中的最新提醒重新进入。'));
        }
        return htmlResponse(400, renderMessagePage('无法处理', '提交内容无效，请返回提醒页面后重试。'));
    }
    throw error;
}

function submittedAction(input: unknown): Record<string, unknown> {
    if (typeof input !== 'object' || input === null || Array.isArray(input)) return {};
    const value = input as Record<string, unknown>;
    const nestedMinutes = value['params.minutes'];
    const params =
        value.params === undefined && (typeof nestedMinutes === 'string' || typeof nestedMinutes === 'number')
            ? { minutes: Number(nestedMinutes) }
            : value.params;
    return {
        action: value.action,
        ...(params === undefined ? {} : { params }),
    };
}

function renderActionPage(token: string, view: Extract<ActionUiView, { state: 'available' }>): string {
    const action = ACTION_UI_ROUTES.execute.replace(':token', encodeURIComponent(token));
    const options = view.options.map((option) => renderActionOption(action, option)).join('');
    return pageShell(
        '处理提醒',
        `<main>
<header class="signal"><span>VoiceLife</span><strong>待处理提醒</strong></header>
<section aria-labelledby="action-title">
<p class="kicker">有效操作</p>
<h1 id="action-title">这条提醒需要你的选择</h1>
<p class="summary">选择后会立即同步到设备。</p>
<div class="actions">${options}</div>
<p class="expiry">操作截止时间 <time datetime="${escapeHtml(view.expiresAt)}">${escapeHtml(formatTime(view.expiresAt))}</time></p>
</section>
</main>`,
    );
}

function renderActionStatePage(view: Exclude<ActionUiView, { state: 'available' }>): string {
    if (view.state === 'submitted') return renderResultPage(view.action, view.params);
    if (view.state === 'processing') {
        return pageShell(
            '设备正在处理',
            '<main class="result"><div class="check" aria-hidden="true">&#8635;</div><p class="kicker">处理中</p><h1>设备正在处理</h1><p class="summary">操作已经送达设备，请稍候查看最终结果。</p></main>',
        );
    }
    if (view.state === 'succeeded') {
        const detail =
            view.action === 'snooze' && view.nextTriggerAt !== undefined
                ? `设备已确认推迟，下一次提醒时间为 ${formatTime(view.nextTriggerAt)}。`
                : view.action === 'snooze' && view.params !== undefined
                  ? `设备已确认推迟 ${String(view.params.minutes)} 分钟。`
                  : '设备已确认这条提醒。';
        return pageShell(
            '提醒已处理',
            `<main class="result"><div class="check" aria-hidden="true">&#10003;</div><p class="kicker">已完成</p><h1>提醒已处理</h1><p class="summary">${escapeHtml(detail)}</p></main>`,
        );
    }
    if (view.state === 'failed') {
        return renderMessagePage('操作未完成', '设备未能完成这次操作，请在设备端查看提醒状态。');
    }
    return renderMessagePage('操作已过期', '这条提醒的操作时间已经结束。');
}

function renderActionOption(actionPath: string, option: ActionUiOption): string {
    const tone = option.action === 'acknowledge' ? 'primary' : 'secondary';
    const minutes =
        option.params === undefined
            ? ''
            : `<input type="hidden" name="params.minutes" value="${String(option.params.minutes)}">`;
    return `<form method="post" action="${escapeHtml(actionPath)}">
<input type="hidden" name="action" value="${option.action}">${minutes}
<button class="${tone}" type="submit">${escapeHtml(option.label)}</button>
</form>`;
}

function renderResultPage(action: ReminderActionCommand['action'], params: ReminderActionCommand['params']): string {
    const detail =
        action === 'snooze' && params !== undefined
            ? `已提交推迟 ${String(params.minutes)} 分钟的请求，等待设备确认。`
            : '操作已提交，等待设备确认。';
    return pageShell(
        '操作已提交',
        `<main class="result"><div class="check" aria-hidden="true">&#10003;</div><p class="kicker">同步中</p><h1>操作已提交</h1><p class="summary">${escapeHtml(detail)}</p></main>`,
    );
}

function renderMessagePage(title: string, detail: string): string {
    return pageShell(
        title,
        `<main class="result error"><div class="check" aria-hidden="true">!</div><p class="kicker">提醒操作</p><h1>${escapeHtml(title)}</h1><p class="summary">${escapeHtml(detail)}</p></main>`,
    );
}

function pageShell(title: string, content: string): string {
    return `<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#f4f7f5">
<title>${escapeHtml(title)} | VoiceLife</title>
<style>
:root{color-scheme:light;--paper:#f4f7f5;--surface:#fff;--ink:#17241e;--muted:#627068;--line:#dce4df;--signal:#087a48;--signal-dark:#075c39;--warm:#b36a16}
*{box-sizing:border-box}
body{margin:0;min-height:100svh;background:var(--paper);color:var(--ink);font-family:system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;letter-spacing:0}
main{width:min(calc(100% - 32px),440px);margin:0 auto;padding:clamp(28px,8vh,72px) 0 max(28px,env(safe-area-inset-bottom))}
.signal{display:flex;align-items:center;justify-content:space-between;border-left:5px solid var(--signal);padding:10px 0 10px 14px;margin-bottom:34px;font-family:ui-rounded,"PingFang SC",sans-serif}
.signal span{color:var(--muted);font-size:13px}.signal strong{font-size:14px}
section{background:var(--surface);border:1px solid var(--line);border-radius:6px;padding:26px 22px}
.kicker{margin:0 0 10px;color:var(--signal-dark);font:700 12px/1.4 ui-monospace,SFMono-Regular,monospace;text-transform:uppercase}
h1{margin:0;font-family:ui-rounded,"PingFang SC",sans-serif;font-size:28px;line-height:1.28;letter-spacing:0;overflow-wrap:anywhere}
.summary{margin:13px 0 0;color:var(--muted);font-size:15px;line-height:1.7}
.actions{display:grid;gap:11px;margin-top:28px}.actions form{margin:0}
button{width:100%;min-height:52px;border:1px solid transparent;border-radius:6px;padding:12px 16px;font:700 16px/1.25 system-ui,-apple-system,"PingFang SC",sans-serif;letter-spacing:0;cursor:pointer}
button:focus-visible{outline:3px solid #e4a64f;outline-offset:3px}.primary{background:var(--signal);color:#fff}.primary:hover{background:var(--signal-dark)}
.secondary{border-color:#b9c9c0;background:#edf4f0;color:var(--signal-dark)}.secondary:hover{border-color:var(--signal);background:#e2eee7}
.expiry{margin:22px 0 0;padding-top:16px;border-top:1px solid var(--line);color:var(--muted);font:12px/1.6 ui-monospace,SFMono-Regular,monospace}
.result{padding-top:clamp(70px,18vh,150px);text-align:center}.result .summary{max-width:340px;margin:13px auto 0}
.check{display:grid;place-items:center;width:58px;height:58px;margin:0 auto 22px;border-radius:50%;background:var(--signal);color:#fff;font:700 28px/1 ui-rounded,sans-serif}
.error .check{background:var(--warm)}
@media (prefers-reduced-motion:no-preference){button{transition:background-color 140ms ease,border-color 140ms ease}}
</style>
</head>
<body>${content}</body>
</html>`;
}

function formatTime(value: string): string {
    return new Intl.DateTimeFormat('zh-CN', {
        timeZone: 'Asia/Shanghai',
        month: 'long',
        day: 'numeric',
        hour: '2-digit',
        minute: '2-digit',
        hour12: false,
    }).format(new Date(value));
}

function escapeHtml(value: string): string {
    return value
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
}
