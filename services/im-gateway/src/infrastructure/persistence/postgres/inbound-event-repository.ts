import type { ChannelAccountId, InboundEventId } from '../../../contracts/ids.js';
import type { InboundEventRecord } from '../../../domain/models.js';
import type { InboundEventRepository } from '../../../ports/repositories.js';
import { mapInboundEvent } from './mappers.js';
import { queryOne, toJson, upsert, type SqlExecutor } from './sql.js';

const INBOUND_COLUMNS = [
    'id',
    'channel_account_id',
    'external_event_id',
    'event_type',
    'payload',
    'status',
    'occurred_at',
    'received_at',
] as const;

/** 规范化入站事件的 PostgreSQL 实现。 */
export class PostgresInboundEventRepository implements InboundEventRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc InboundEventRepository.findById} */
    public async findById(id: InboundEventId): Promise<InboundEventRecord | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_inbound_events WHERE id = $1', [id]);
        return row === undefined ? undefined : mapInboundEvent(row);
    }

    /** {@inheritDoc InboundEventRepository.findByExternalEvent} */
    public async findByExternalEvent(
        channelAccountId: ChannelAccountId,
        externalEventId: string,
    ): Promise<InboundEventRecord | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_inbound_events WHERE channel_account_id = $1 AND external_event_id = $2 LIMIT 1',
            [channelAccountId, externalEventId],
        );
        return row === undefined ? undefined : mapInboundEvent(row);
    }

    /** {@inheritDoc InboundEventRepository.save} */
    public async save(event: InboundEventRecord): Promise<void> {
        await upsert(
            this.executor,
            'im_inbound_events',
            INBOUND_COLUMNS,
            [
                event.id,
                event.channelAccountId,
                event.externalEventId,
                event.eventType,
                toJson(event.payload),
                event.status,
                event.occurredAt,
                event.receivedAt,
            ],
            ['channel_account_id', 'external_event_id'],
        );
    }
}
