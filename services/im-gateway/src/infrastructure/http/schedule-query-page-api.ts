import type { ScheduleQueryPageApplication } from '../../application/api.js';
import type { ScheduleQueryResultIntent } from '../../contracts/device-gateway.js';
import { parseActionToken } from '../../contracts/device-gateway-parser.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { JsonValue } from '../../shared/types.js';
import type { ActionUiPageResponse } from './action-ui-api.js';

/** 日程查询结果的只读 H5 路由。 */
export const SCHEDULE_QUERY_PAGE_ROUTE = '/voicelife/reminder-actions/query-result/:token' as const;

/** 服务端渲染的日程查询结果页面。 */
export class ScheduleQueryPageController {
    /** @param scheduleQueries 日程查询只读页面服务。 */
    public constructor(private readonly scheduleQueries: ScheduleQueryPageApplication) {}

    /**
     * 校验路径令牌并呈现完整日程清单。
     * @param token 未受信任的路径令牌。
     * @returns 不含写入能力的 HTML 页面响应。
     */
    public async get(token: unknown): Promise<ActionUiPageResponse> {
        try {
            return htmlResponse(200, renderScheduleQueryPage(await this.scheduleQueries.show(parseActionToken(token))));
        } catch (error) {
            return queryPageErrorResponse(error);
        }
    }
}

const PAGE_HEADERS = {
    'content-type': 'text/html; charset=utf-8',
    'cache-control': 'no-store',
    'content-security-policy': "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'",
    'referrer-policy': 'no-referrer',
    'x-content-type-options': 'nosniff',
    'strict-transport-security': 'max-age=31536000; includeSubDomains',
    'permissions-policy': 'camera=(), microphone=(), geolocation=()',
} as const;

function htmlResponse(status: ActionUiPageResponse['status'], body: string): ActionUiPageResponse {
    return { status, headers: { ...PAGE_HEADERS }, body };
}

function queryPageErrorResponse(error: unknown): ActionUiPageResponse {
    if (error instanceof ImGatewayError) {
        if (error.code === 'action_expired') {
            return htmlResponse(410, renderMessagePage('查询链接已过期', '请在设备上重新查询日程。'));
        }
        if (error.code === 'action_not_found') {
            return htmlResponse(404, renderMessagePage('查询结果不可用', '请从微信中的最新日程查询消息重新进入。'));
        }
        return htmlResponse(400, renderMessagePage('无法打开查询结果', '链接内容无效，请返回微信后重试。'));
    }
    throw error;
}

function renderScheduleQueryPage(intent: ScheduleQueryResultIntent): string {
    const entries = [...intent.schedules, ...intent.futureOccurrences];
    const list =
        entries.length === 0
            ? '<section class="empty"><p class="empty-mark" aria-hidden="true">0</p><h2>没有匹配的日程</h2></section>'
            : `<ol class="agenda">${entries.map((entry) => renderScheduleEntry(entry)).join('')}</ol>`;
    const queryDetails = [
        `范围 ${formatQueryRange(intent.query.startDate, intent.query.endDate)}`,
        `状态 ${statusLabel(intent.query.status)}`,
        ...(intent.query.keyword === undefined ? [] : [`关键词 ${intent.query.keyword}`]),
    ];
    return pageShell(
        '日程清单',
        `<main>
<header class="masthead"><span class="brand">VoiceLife</span><time datetime="${escapeHtml(intent.queriedAt)}">${escapeHtml(formatQueryTime(intent.queriedAt))}</time></header>
<section class="intro" aria-labelledby="page-title"><p class="eyebrow">日程查询</p><h1 id="page-title">${intent.resultCount} 条日程</h1><p class="scope">${escapeHtml(formatQueryRange(intent.query.startDate, intent.query.endDate))}</p></section>
${list}
${intent.exceptions.length === 0 ? '' : `<p class="exception">包含 ${String(intent.exceptions.length)} 项例外调整</p>`}
<footer>${queryDetails.map((detail) => `<span>${escapeHtml(detail)}</span>`).join('')}</footer>
</main>`,
    );
}

function renderScheduleEntry(value: JsonValue): string {
    const title = field(value, 'event') ?? '未命名日程';
    const start = formatScheduleTime(field(value, 'start_time') ?? field(value, 'original_start_time'));
    const end = formatScheduleTime(field(value, 'end_time'));
    const location = field(value, 'location');
    const notes = field(value, 'notes');
    const time = start === undefined ? '时间待定' : end === undefined ? start : `${start} - ${end}`;
    return `<li><div class="time">${escapeHtml(time)}</div><article><h2>${escapeHtml(title)}</h2>${location === undefined ? '' : `<p class="place">${escapeHtml(location)}</p>`}${notes === undefined ? '' : `<p class="notes">${escapeHtml(notes)}</p>`}</article></li>`;
}

function field(value: JsonValue, key: string): string | undefined {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) return undefined;
    const item = value[key];
    return typeof item === 'string' && item.trim() !== '' ? item.trim().replace(/\s+/gu, ' ') : undefined;
}

function formatScheduleTime(value: string | undefined): string | undefined {
    if (value === undefined) return undefined;
    const match = /^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2})(?::\d{2})?$/u.exec(value);
    if (match === null) return value;
    return `${Number(match[2])}月${Number(match[3])}日 ${match[4]}:${match[5]}`;
}

function formatQueryRange(startDate: string | undefined, endDate: string | undefined): string {
    if (startDate !== undefined && endDate !== undefined) return `${startDate} 至 ${endDate}`;
    if (startDate !== undefined) return `${startDate} 起`;
    if (endDate !== undefined) return `截至 ${endDate}`;
    return '全部时间';
}

function statusLabel(status: ScheduleQueryResultIntent['query']['status']): string {
    return { all: '全部', active: '进行中', cancelled: '已取消', completed: '已完成' }[status];
}

function formatQueryTime(value: string): string {
    return new Intl.DateTimeFormat('zh-CN', {
        timeZone: 'Asia/Shanghai',
        month: 'numeric',
        day: 'numeric',
        hour: '2-digit',
        minute: '2-digit',
        hour12: false,
    }).format(new Date(value));
}

function renderMessagePage(title: string, detail: string): string {
    return pageShell(
        title,
        `<main class="message"><p class="eyebrow">日程查询</p><h1>${escapeHtml(title)}</h1><p>${escapeHtml(detail)}</p></main>`,
    );
}

function pageShell(title: string, content: string): string {
    return `<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#f4f8f7">
<title>${escapeHtml(title)} | VoiceLife</title>
<style>
:root{color-scheme:light;--paper:#f4f8f7;--surface:#ffffff;--ink:#16332e;--muted:#60746f;--line:#d7e3df;--green:#087a5a;--green-soft:#e4f1ed;--orange:#d77922;--blue:#23758b}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font-family:system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;letter-spacing:0}main{width:min(calc(100% - 32px),640px);margin:0 auto;padding:24px 0 max(32px,env(safe-area-inset-bottom))}.masthead{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--line);padding:0 0 16px}.brand{font:700 14px/1 ui-rounded,"PingFang SC",sans-serif;color:var(--green)}.masthead time{color:var(--muted);font:12px/1 ui-monospace,SFMono-Regular,monospace}.intro{padding:30px 0 26px}.eyebrow{margin:0 0 10px;color:var(--green);font:700 12px/1.3 ui-monospace,SFMono-Regular,monospace;letter-spacing:0}h1,h2,p{overflow-wrap:anywhere}h1{margin:0;font:700 34px/1.2 ui-rounded,"PingFang SC",sans-serif}.scope{margin:12px 0 0;color:var(--muted);font-size:15px;line-height:1.5}.agenda{list-style:none;margin:0;padding:0}.agenda li{display:grid;grid-template-columns:104px minmax(0,1fr);gap:14px;border-top:1px solid var(--line);padding:18px 0}.agenda li:last-child{border-bottom:1px solid var(--line)}.time{position:relative;padding-left:13px;color:var(--blue);font:700 13px/1.45 ui-monospace,SFMono-Regular,monospace}.time::before{content:"";position:absolute;left:0;top:3px;width:4px;height:28px;background:var(--orange);border-radius:2px}.agenda article{min-width:0}.agenda h2{margin:0;font-size:18px;line-height:1.38}.place,.notes{margin:7px 0 0;font-size:14px;line-height:1.55}.place{color:var(--green)}.notes{color:var(--muted)}.exception{margin:20px 0 0;padding:12px 14px;border-left:4px solid var(--orange);background:#fff8ef;color:#77511f;font-size:14px;line-height:1.5}.empty{border:1px solid var(--line);border-radius:6px;background:var(--surface);padding:34px 24px;text-align:center}.empty-mark{margin:0 0 10px;color:var(--green);font:700 46px/1 ui-monospace,SFMono-Regular,monospace}.empty h2{margin:0;font-size:18px}footer{display:flex;flex-wrap:wrap;gap:8px;margin-top:24px;color:var(--muted);font-size:12px;line-height:1.4}footer span{border:1px solid var(--line);border-radius:999px;padding:6px 9px;background:var(--surface)}.message{padding-top:18vh}.message h1{font-size:30px}.message p:not(.eyebrow){max-width:320px;color:var(--muted);font-size:16px;line-height:1.7}@media (max-width:420px){main{width:min(calc(100% - 24px),640px)}.agenda li{grid-template-columns:88px minmax(0,1fr);gap:10px}.time{font-size:12px}h1{font-size:31px}}@media (prefers-reduced-motion:no-preference){.agenda li{transition:background-color 160ms ease}.agenda li:hover{background:var(--green-soft)}}
</style>
</head>
<body>${content}</body>
</html>`;
}

function escapeHtml(value: string): string {
    return value
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
}
