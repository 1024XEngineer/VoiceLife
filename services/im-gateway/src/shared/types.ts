/** 为基础类型附加仅在编译期存在的名义类型标记。 */
export type Brand<T, Name extends string> = T & {
    readonly __brand: Name;
};

/** ISO 8601 格式的日期时间字符串。 */
export type IsoDateTime = Brand<string, 'IsoDateTime'>;

/** 可无损序列化为 JSON 的值。 */
export type JsonValue = null | boolean | number | string | JsonValue[] | { readonly [key: string]: JsonValue };

/** 基于游标的分页请求参数。 */
export interface PageRequest {
    readonly cursor?: string;
    readonly limit: number;
}

/** 基于游标的只读分页结果。 */
export interface Page<T> {
    readonly items: readonly T[];
    readonly nextCursor?: string;
}
