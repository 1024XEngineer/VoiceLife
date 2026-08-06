import { Pool } from 'pg';
import type { ImUnitOfWork, ImUnitOfWorkContext } from '../../ports/repositories.js';
import type { DbRow } from './postgres/mappers.js';
import { PostgresUnitOfWorkContext, type SqlExecutor } from './postgres/repositories.js';
import { applySchema, IM_TABLES } from './postgres/schema.js';

/** 可执行参数化 SQL 的 pg 查询接口。 */
type Queryable = {
    query(text: string, values?: readonly unknown[]): Promise<{ rows: readonly DbRow[] }>;
};

/** 将 pg 连接池或事务客户端适配为仓储使用的执行器。 */
function toExecutor(queryable: Queryable): SqlExecutor {
    return queryable;
}

/**
 * PostgreSQL 持久化工作单元。
 *
 * 使用 node-postgres 直连与手写参数化 SQL，为投递、尝试、回执、动作和
 * 事务性发件箱提供真实的跨聚合事务。跨边界 ID 统一使用不透明字符串
 * 主键，不引入内部整数主键；时间统一按 UTC 的 timestamptz 持久化。
 */
export class PostgresImUnitOfWork implements ImUnitOfWork {
    private readonly pool: Pool;

    /** @param connectionString PostgreSQL 连接字符串。 */
    public constructor(public readonly connectionString: string) {
        this.pool = new Pool({
            connectionString,
            connectionTimeoutMillis: 2000,
            max: 10,
        });
    }

    /**
     * 在同一原子事务内执行跨聚合工作。
     * @param work 使用事务仓储上下文的回调。
     * @returns 回调结果；任何失败都会回滚整笔事务。
     */
    public async transaction<T>(work: (context: ImUnitOfWorkContext) => Promise<T>): Promise<T> {
        const client = await this.pool.connect();
        try {
            await client.query('BEGIN');
            const context = new PostgresUnitOfWorkContext(toExecutor(client));
            try {
                const result = await work(context);
                await client.query('COMMIT');
                return result;
            } catch (error) {
                await client.query('ROLLBACK');
                throw error;
            }
        } finally {
            client.release();
        }
    }

    /**
     * 幂等应用 IM Gateway 所需的表结构与索引。
     * @returns 迁移完成后兑现的 Promise。
     */
    public async migrate(): Promise<void> {
        await applySchema(toExecutor(this.pool));
    }

    /**
     * 清空全部 IM 表，供测试与诊断使用。
     * @returns 清空完成后兑现的 Promise。
     */
    public async truncateAll(): Promise<void> {
        const tables = IM_TABLES.map((table) => `"${table}"`).join(', ');
        await this.pool.query(`TRUNCATE ${tables} RESTART IDENTITY CASCADE`);
    }

    /**
     * 释放连接池。
     * @returns 关闭完成后兑现的 Promise。
     */
    public close(): Promise<void> {
        return this.pool.end();
    }

    /**
     * 执行一条诊断用的原始查询。
     * @param text 参数化 SQL。
     * @param params 绑定参数。
     * @returns 查询返回的行数组。
     */
    public async runRaw<T = Record<string, unknown>>(text: string, params?: readonly unknown[]): Promise<T[]> {
        const result = await this.pool.query(text, params as unknown[]);
        return result.rows as T[];
    }
}
