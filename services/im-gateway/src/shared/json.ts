import type { JsonValue } from './types.js';

/**
 * 为请求级幂等检查生成稳定且无损的 JSON 表示。
 * @param value 要序列化的 JSON 兼容值。
 * @returns 对象键经过递归排序的规范 JSON。
 */
export function canonicalizeJson(value: JsonValue): string {
    if (Array.isArray(value)) {
        return `[${value.map(canonicalizeJson).join(',')}]`;
    }
    if (value !== null && typeof value === 'object') {
        return `{${Object.keys(value)
            .sort()
            .map((key) => `${JSON.stringify(key)}:${canonicalizeJson(value[key]!)}`)
            .join(',')}}`;
    }
    const encoded = JSON.stringify(value);
    if (encoded === undefined) {
        throw new TypeError('JsonValue cannot contain undefined');
    }
    return encoded;
}
