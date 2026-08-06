import type { ImOutboxEvent } from '../../../domain/models.js';
import type { OutboxRepository } from '../../../ports/repositories.js';
import { toJson, type SqlExecutor } from './sql.js';

const OUTBOX_COLUMNS = [
    'id',
    'event_type',
    'aggregate_id',
    'payload',
    'status',
    'attempts',
    'available_at',
    'created_at',
    'published_at',
] as const;

/** 事务性发件箱的 PostgreSQL 实现。 */
export class PostgresOutboxRepository implements OutboxRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc OutboxRepository.append} */
    public async append(event: ImOutboxEvent): Promise<void> {
        await this.executor.query(
            `INSERT INTO im_outbox_events (${OUTBOX_COLUMNS.map((column) => `"${column}"`).join(', ')})
             VALUES (${OUTBOX_COLUMNS.map((_, index) => `$${index + 1}`).join(', ')})`,
            [
                event.id,
                event.eventType,
                event.aggregateId,
                toJson(event.payload),
                event.status,
                event.attempts,
                event.availableAt,
                event.createdAt,
                event.publishedAt ?? null,
            ],
        );
    }
}
