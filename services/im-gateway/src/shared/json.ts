import type { JsonValue } from './types.js';

/** Stable, lossless representation used for request-level idempotency checks. */
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
