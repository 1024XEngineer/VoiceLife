import type { EventId } from '../../../contracts/ids.js';
import type { IntentSubmissionRecord } from '../../../domain/models.js';
import type { IntentSubmissionRepository } from '../../../ports/repositories.js';
import { mapIntentSubmission } from './mappers.js';
import { queryOne, toJson, upsert, type SqlExecutor } from './sql.js';

const INTENT_COLUMNS = ['business_event_id', 'kind', 'request_fingerprint', 'submission', 'created_at'] as const;

/** 请求级幂等记录的 PostgreSQL 实现。 */
export class PostgresIntentSubmissionRepository implements IntentSubmissionRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc IntentSubmissionRepository.findByBusinessKey} */
    public async findByBusinessKey(
        businessEventId: EventId,
        kind: IntentSubmissionRecord['kind'],
    ): Promise<IntentSubmissionRecord | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_intent_submissions WHERE business_event_id = $1 AND kind = $2 LIMIT 1',
            [businessEventId, kind],
        );
        return row === undefined ? undefined : mapIntentSubmission(row);
    }

    /** {@inheritDoc IntentSubmissionRepository.save} */
    public async save(record: IntentSubmissionRecord): Promise<void> {
        await upsert(
            this.executor,
            'im_intent_submissions',
            INTENT_COLUMNS,
            [
                record.businessEventId,
                record.kind,
                record.requestFingerprint,
                toJson(record.submission),
                record.createdAt,
            ],
            ['business_event_id', 'kind'],
        );
    }
}
