import type { OutboxEventId } from '../../../contracts/ids.js';
import type { ImOutboxEvent } from '../../../domain/models.js';
import type { OutboxRepository } from '../../../ports/repositories.js';
import type { IsoDateTime } from '../../../shared/types.js';
import { mapOutboxEvent } from './mappers.js';
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

    /** {@inheritDoc OutboxRepository.claimPending} */
    public async claimPending(
        eventTypes: readonly string[],
        now: IsoDateTime,
        leaseUntil: IsoDateTime,
        limit: number,
    ): Promise<readonly ImOutboxEvent[]> {
        const { rows } = await this.executor.query(
            `WITH due AS (
                SELECT id
                FROM im_outbox_events
                WHERE status = 'pending'
                  AND event_type = ANY($1::text[])
                  AND available_at <= $2
                ORDER BY available_at, created_at, id
                FOR UPDATE SKIP LOCKED
                LIMIT $4
             )
             UPDATE im_outbox_events AS event
             SET attempts = event.attempts + 1,
                 available_at = $3
             FROM due
             WHERE event.id = due.id
             RETURNING ${OUTBOX_COLUMNS.map((column) => `event."${column}"`).join(', ')}`,
            [eventTypes, now, leaseUntil, limit],
        );
        return rows.map(mapOutboxEvent);
    }

    /** {@inheritDoc OutboxRepository.markPublished} */
    public async markPublished(eventId: OutboxEventId, publishedAt: IsoDateTime): Promise<void> {
        await this.executor.query(
            `UPDATE im_outbox_events
             SET status = 'published', published_at = $2
             WHERE id = $1 AND status = 'pending'`,
            [eventId, publishedAt],
        );
    }

    /** {@inheritDoc OutboxRepository.markFailed} */
    public async markFailed(eventId: OutboxEventId): Promise<void> {
        await this.executor.query(
            `UPDATE im_outbox_events SET status = 'failed' WHERE id = $1 AND status = 'pending'`,
            [eventId],
        );
    }
}
