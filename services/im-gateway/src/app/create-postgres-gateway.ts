import type { ImGatewayDependencies, ImGatewayRuntime } from './create-im-gateway.js';
import { createImGateway } from './create-im-gateway.js';
import { PostgresImUnitOfWork } from '../infrastructure/persistence/postgres.js';

/** 与 docker-compose.yml 及测试 fixtures 一致的本地默认连接地址。 */
const DEFAULT_POSTGRES_URL = 'postgres://voicelife:voicelife@localhost:5432/voicelife';

/** 以 PostgreSQL 为持久层的生产组合根配置。 */
export interface PostgresGatewayOptions {
    /**
     * PostgreSQL 连接地址；缺省时读取 DATABASE_URL 环境变量，再缺省回落到本地 docker-compose 地址。
     * @default process.env.DATABASE_URL ?? DEFAULT_POSTGRES_URL
     */
    readonly databaseUrl?: string;
    /** 除持久化工作单元外的全部外部端口。 */
    readonly ports: Omit<ImGatewayDependencies, 'unitOfWork'>;
}

/** 已接管连接池生命周期的 Postgres 版 Gateway 运行时。 */
export interface PostgresGatewayRuntime {
    /** 可供传输层承载的 Gateway 运行时。 */
    readonly runtime: ImGatewayRuntime;
    /**
     * 释放连接池，应在进程退出或优雅停机时调用。
     * @returns 释放完成后兑现的 Promise。
     */
    close(): Promise<void>;
}

/**
 * 以 PostgreSQL 为持久层装配生产 Gateway：解析连接地址、执行 schema 迁移并托管连接池。
 * @param options 连接地址与外部端口。
 * @returns 可承载传输层的运行时与连接池关闭句柄。
 */
export async function createPostgresImGateway(options: PostgresGatewayOptions): Promise<PostgresGatewayRuntime> {
    const databaseUrl = options.databaseUrl ?? process.env.DATABASE_URL ?? DEFAULT_POSTGRES_URL;
    const unitOfWork = new PostgresImUnitOfWork(databaseUrl);
    await unitOfWork.migrate();
    const runtime = createImGateway({ unitOfWork, ...options.ports });
    return {
        runtime,
        close: () => unitOfWork.close(),
    };
}
