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

    /** {@inheritDoc IntentSubmissionRepository.createIfAbsent} */
    public async createIfAbsent(
        record: IntentSubmissionRecord,
    ): Promise<{ created: boolean; record: IntentSubmissionRecord }> {
        const quoted = INTENT_COLUMNS.map((column) => `"${column}"`).join(', ');
        const placeholders = INTENT_COLUMNS.map((_, index) => `$${index + 1}`).join(', ');
        const inserted = await queryOne(
            this.executor,
            `INSERT INTO im_intent_submissions (${quoted}) VALUES (${placeholders})
             ON CONFLICT (business_event_id, kind) DO NOTHING
             RETURNING *`,
            this.toRow(record),
        );
        if (inserted !== undefined) return { created: true, record: mapIntentSubmission(inserted) };
        const existing = await this.findByBusinessKey(record.businessEventId, record.kind);
        if (existing === undefined) {
            // 仅当冲突行提交后业务键不可见时触发，属不变量异常。
            throw new Error('im_intent_submissions business key vanished after idempotent insert');
        }
        return { created: false, record: existing };
    }

    /** {@inheritDoc IntentSubmissionRepository.finalizeClaim} */
    public async finalizeClaim(record: IntentSubmissionRecord): Promise<void> {
        await upsert(this.executor, 'im_intent_submissions', INTENT_COLUMNS, this.toRow(record), [
            'business_event_id',
            'kind',
        ]);
    }

    /** 将受理记录映射为与 INTENT_COLUMNS 一一对应的参数行。 */
    private toRow(record: IntentSubmissionRecord): readonly unknown[] {
        return [
            record.businessEventId,
            record.kind,
            record.requestFingerprint,
            toJson(record.submission),
            record.createdAt,
        ];
    }
}
