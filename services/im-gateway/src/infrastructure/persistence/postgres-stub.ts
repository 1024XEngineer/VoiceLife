import type { ImUnitOfWork, ImUnitOfWorkContext } from '../../ports/repositories.js';
import { ImGatewayError } from '../../shared/errors.js';

/**
 * 生产环境持久化边界。
 *
 * 最终实现使用 PostgreSQL 与 Kysely，并为投递、尝试、回执、动作和
 * 事务性发件箱提供真正的跨聚合事务。
 */
export class PostgresImUnitOfWork implements ImUnitOfWork {
    /** @param connectionString PostgreSQL 连接字符串。 */
    public constructor(public readonly connectionString: string) {}

    /** {@inheritDoc ImUnitOfWork.transaction} */
    public transaction<T>(_work: (context: ImUnitOfWorkContext) => Promise<T>): Promise<T> {
        return Promise.reject(
            new ImGatewayError('not_implemented', 'PostgreSQL adapter is an empty architecture skeleton'),
        );
    }
}
