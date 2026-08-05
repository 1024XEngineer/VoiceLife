import type { ImUnitOfWork, ImUnitOfWorkContext } from '../../ports/repositories.js';
import { ImGatewayError } from '../../shared/errors.js';

/**
 * Production persistence seam.
 *
 * The implementation will use PostgreSQL + Kysely and must provide a real
 * transaction spanning Delivery, Attempt, Receipt, Action and ImOutboxEvent.
 */
export class PostgresImUnitOfWork implements ImUnitOfWork {
    public constructor(public readonly connectionString: string) {}

    public transaction<T>(_work: (context: ImUnitOfWorkContext) => Promise<T>): Promise<T> {
        return Promise.reject(
            new ImGatewayError('not_implemented', 'PostgreSQL adapter is an empty architecture skeleton'),
        );
    }
}
