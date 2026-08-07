import type { WechatTemplateFields } from './wechat-official-outbound.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { JsonValue } from '../../shared/types.js';

const MAX_API_RESPONSE_BYTES = 64 * 1024;
const TEXT_ENCODER = new TextEncoder();

/** 解析并校验微信模板消息载荷。 */
export function parseTemplatePayload(
    value: JsonValue,
    expectedTemplateId: string,
    expectedTemplateFields: WechatTemplateFields,
    actionUiBaseUrl: string,
): {
    readonly type: 'wechat_template';
    readonly templateId: string;
    readonly data: Readonly<Record<string, { readonly value: string }>>;
    readonly url?: string;
} {
    if (!isRecord(value) || value.type !== 'wechat_template') {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound payload is invalid');
    }
    const templateId = requiredString(value.templateId, 'templateId');
    if (templateId !== expectedTemplateId) {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound template id does not match configuration');
    }
    const expectedFields = Object.values(expectedTemplateFields);
    const payloadData = value.data;
    if (!isRecord(payloadData)) {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound template data is invalid');
    }
    if (
        Object.keys(payloadData).length !== expectedFields.length ||
        !expectedFields.every((field) => Object.hasOwn(payloadData, field))
    ) {
        throw new ImGatewayError('invalid_contract', 'WeChat outbound template data is invalid');
    }
    const data: Record<string, { readonly value: string }> = {};
    for (const [field, item] of Object.entries(payloadData)) {
        if (!/^[A-Za-z][A-Za-z0-9_]{0,63}$/u.test(field) || !isRecord(item) || typeof item.value !== 'string') {
            throw new ImGatewayError('invalid_contract', 'WeChat outbound template data is invalid');
        }
        data[field] = { value: item.value };
    }
    const url = value.url === undefined ? undefined : requiredString(value.url, 'url');
    if (url !== undefined) {
        try {
            const parsed = new URL(url);
            const base = new URL(actionUiBaseUrl);
            if (
                parsed.protocol !== 'https:' ||
                parsed.username !== '' ||
                parsed.password !== '' ||
                parsed.search !== '' ||
                parsed.hash !== '' ||
                parsed.origin !== base.origin ||
                !parsed.pathname.startsWith(`${base.pathname.replace(/\/$/u, '')}/`)
            ) {
                throw new Error('unsafe action UI URL');
            }
        } catch {
            throw new ImGatewayError('invalid_contract', 'WeChat outbound Action UI URL must use HTTPS');
        }
    }
    return { type: 'wechat_template', templateId, data, ...(url === undefined ? {} : { url }) };
}

/** 读取并校验微信 API JSON 响应，同时保留顶层 msgid 的精确十进制文本。 */
export async function readWechatApiResponse(response: Response): Promise<{
    readonly errcode: number;
    readonly msgid?: string;
    readonly accessToken?: string;
    readonly expiresIn?: number;
}> {
    const contentLength = response.headers.get('content-length');
    if (contentLength !== null && Number(contentLength) > MAX_API_RESPONSE_BYTES) {
        throw protocolError('WeChat API response exceeded the size limit');
    }
    const raw = await response.text();
    if (TEXT_ENCODER.encode(raw).byteLength > MAX_API_RESPONSE_BYTES) {
        throw protocolError('WeChat API response exceeded the size limit');
    }
    let parsed: unknown;
    try {
        parsed = JSON.parse(raw);
    } catch {
        throw protocolError('WeChat API returned invalid JSON');
    }
    if (!isRecord(parsed)) throw protocolError('WeChat API returned an invalid object');
    const errcode = parsed.errcode === undefined ? 0 : parsed.errcode;
    if (typeof errcode !== 'number' || !Number.isSafeInteger(errcode)) {
        throw protocolError('WeChat API returned an invalid error code');
    }
    const rawMessageId = topLevelJsonProperty(raw, 'msgid');
    let msgid: string | undefined;
    if (rawMessageId !== undefined) {
        if (rawMessageId.startsWith('"')) {
            const stringValue = JSON.parse(rawMessageId) as unknown;
            if (typeof stringValue !== 'string' || !/^[0-9]{1,64}$/u.test(stringValue)) {
                throw protocolError('WeChat API returned an invalid message id');
            }
            msgid = stringValue;
        } else if (/^[0-9]{1,64}$/u.test(rawMessageId)) {
            msgid = rawMessageId;
        } else {
            throw protocolError('WeChat API returned an invalid message id');
        }
    }
    const accessToken = typeof parsed.access_token === 'string' ? parsed.access_token : undefined;
    const expiresIn =
        typeof parsed.expires_in === 'number' && Number.isSafeInteger(parsed.expires_in) && parsed.expires_in > 0
            ? parsed.expires_in
            : undefined;
    return {
        errcode,
        ...(msgid === undefined ? {} : { msgid }),
        ...(accessToken === undefined ? {} : { accessToken }),
        ...(expiresIn === undefined ? {} : { expiresIn }),
    };
}

/** 使用 AbortController 为微信 HTTP 请求设置截止时间。 */
export async function fetchWithTimeout(
    fetchImpl: typeof fetch,
    url: URL,
    init: RequestInit,
    timeoutMs: number,
): Promise<Response> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
        return await fetchImpl(url, { ...init, signal: controller.signal });
    } finally {
        clearTimeout(timer);
    }
}

class WechatProtocolError extends Error {
    public constructor(message: string) {
        super(message);
        this.name = 'WechatProtocolError';
    }
}

function protocolError(message: string): Error {
    return new WechatProtocolError(message);
}

function requiredString(value: unknown, field: string): string {
    if (typeof value !== 'string' || value.trim() === '') {
        throw new ImGatewayError('invalid_contract', `WeChat outbound ${field} is invalid`);
    }
    return value;
}

function topLevelJsonProperty(raw: string, property: string): string | undefined {
    let index = skipWhitespace(raw, 0);
    if (raw[index] !== '{') return undefined;
    index = skipWhitespace(raw, index + 1);
    while (index < raw.length && raw[index] !== '}') {
        if (raw[index] !== '"') throw protocolError('WeChat API returned an invalid object');
        const keyEnd = readJsonStringEnd(raw, index);
        const key = JSON.parse(raw.slice(index, keyEnd)) as unknown;
        index = skipWhitespace(raw, keyEnd);
        if (raw[index] !== ':') throw protocolError('WeChat API returned an invalid object');
        const valueStart = skipWhitespace(raw, index + 1);
        const valueEnd = skipJsonValue(raw, valueStart);
        if (key === property) return raw.slice(valueStart, valueEnd).trim();
        index = skipWhitespace(raw, valueEnd);
        if (raw[index] === ',') index = skipWhitespace(raw, index + 1);
    }
    return undefined;
}

function skipJsonValue(raw: string, start: number): number {
    if (raw[start] === '"') return readJsonStringEnd(raw, start);
    if (raw[start] !== '{' && raw[start] !== '[') {
        let index = start;
        while (index < raw.length && raw[index] !== ',' && raw[index] !== '}') index += 1;
        return index;
    }
    const stack: string[] = [raw[start]!];
    let index = start + 1;
    while (index < raw.length && stack.length > 0) {
        if (raw[index] === '"') {
            index = readJsonStringEnd(raw, index);
            continue;
        }
        if (raw[index] === '{' || raw[index] === '[') stack.push(raw[index]!);
        if (raw[index] === '}' || raw[index] === ']') stack.pop();
        index += 1;
    }
    if (stack.length > 0) throw protocolError('WeChat API returned an invalid JSON value');
    return index;
}

function readJsonStringEnd(raw: string, start: number): number {
    let index = start + 1;
    while (index < raw.length) {
        if (raw[index] === '\\') {
            index += 2;
            continue;
        }
        if (raw[index] === '"') return index + 1;
        index += 1;
    }
    throw protocolError('WeChat API returned an invalid JSON string');
}

function skipWhitespace(raw: string, start: number): number {
    let index = start;
    while (index < raw.length && /\s/u.test(raw[index]!)) index += 1;
    return index;
}

function isRecord(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}
