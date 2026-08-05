export type Brand<T, Name extends string> = T & {
    readonly __brand: Name;
};

export type IsoDateTime = Brand<string, 'IsoDateTime'>;

export type JsonValue = null | boolean | number | string | JsonValue[] | { readonly [key: string]: JsonValue };

export interface PageRequest {
    readonly cursor?: string;
    readonly limit: number;
}

export interface Page<T> {
    readonly items: readonly T[];
    readonly nextCursor?: string;
}
