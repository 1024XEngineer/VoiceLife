import type { DbRow } from './mappers.js';

/** 可执行参数化 SQL 的最小接口，由连接池或事务客户端实现。 */
export interface SqlExecutor {
    /**
     * 执行一条参数化查询。
     * @param text 参数化 SQL。
     * @param values 绑定参数。
     * @returns 查询结果行。
     */
    query(text: string, values?: readonly unknown[]): Promise<{ rows: readonly DbRow[] }>;
}

/**
 * 按冲突列做整行 upsert：冲突时把非冲突列替换为本次写入值。
 * @param executor SQL 执行器。
 * @param table 目标表名。
 * @param columns 全部列名。
 * @param row 与列一一对应的参数值。
 * @param conflict 唯一冲突列。
 * @returns 写入完成后兑现的 Promise。
 */
export async function upsert(
    executor: SqlExecutor,
    table: string,
    columns: readonly string[],
    row: readonly unknown[],
    conflict: readonly string[],
): Promise<void> {
    const quoted = columns.map((column) => `"${column}"`).join(', ');
    const placeholders = columns.map((_, index) => `$${index + 1}`).join(', ');
    const conflictTarget = conflict.map((column) => `"${column}"`).join(', ');
    const updates = columns
        .filter((column) => !conflict.includes(column))
        .map((column) => `"${column}" = EXCLUDED."${column}"`)
        .join(', ');
    await executor.query(
        `INSERT INTO "${table}" (${quoted}) VALUES (${placeholders}) ON CONFLICT (${conflictTarget}) DO UPDATE SET ${updates}`,
        row,
    );
}

/**
 * 将 JSON 值序列化为 jsonb 参数；pg 会把 JS 数组特殊处理为数组字面量，必须显式序列化。
 * @param value 领域模型中的 JSON 值。
 * @returns 序列化后的 JSON 字符串，空值返回 null。
 */
export function toJson(value: unknown): string | null {
    return value === undefined || value === null ? null : JSON.stringify(value);
}

/**
 * 执行查询并返回首行，无结果时返回 undefined。
 * @param executor SQL 执行器。
 * @param sql SQL 语句。
 * @param params 参数。
 * @returns 查询结果行。
 */
export async function queryOne(
    executor: SqlExecutor,
    sql: string,
    params: readonly unknown[],
): Promise<DbRow | undefined> {
    const { rows } = await executor.query(sql, params);
    return rows[0];
}
