import type { DeviceId, EventId, OperationId, ReminderTriggerId } from '../../../contracts/ids.js';
import type { DeviceReminderActionFact } from '../../../domain/models.js';
import type { ReminderActionFactRepository } from '../../../ports/repositories.js';
import { mapReminderActionFact } from './mappers.js';
import { queryOne, toJson, upsert, type SqlExecutor } from './sql.js';
import type { IsoDateTime } from '../../../shared/types.js';

const FACT_COLUMNS = [
    'event_id',
    'fingerprint',
    'schema_version',
    'correlation_id',
    'device_id',
    'reminder_trigger_id',
    'operation_id',
    'action',
    'status',
    'occurred_at',
    'next_trigger_at',
    'error_code',
    'details',
    'received_at',
] as const;

/** 设备语音动作事实的 PostgreSQL 持久化实现。 */
export class PostgresReminderActionFactRepository implements ReminderActionFactRepository {
    /**
     * 创建动作事实仓储。
     * @param executor 当前事务或连接的 SQL 执行器。
     */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc ReminderActionFactRepository.findByEventId} */
    public async findByEventId(eventId: EventId): Promise<DeviceReminderActionFact | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_reminder_action_facts WHERE event_id = $1', [
            eventId,
        ]);
        return row === undefined ? undefined : mapReminderActionFact(row);
    }

    /** {@inheritDoc ReminderActionFactRepository.findByDeviceAndOperationId} */
    public async findByDeviceAndOperationId(
        deviceId: DeviceId,
        operationId: OperationId,
    ): Promise<DeviceReminderActionFact | undefined> {
        const row = await queryOne(
            this.executor,
            `SELECT * FROM im_reminder_action_facts
             WHERE device_id = $1 AND operation_id = $2
             LIMIT 1`,
            [deviceId, operationId],
        );
        return row === undefined ? undefined : mapReminderActionFact(row);
    }

    /** {@inheritDoc ReminderActionFactRepository.findByDeviceTriggerAndOccurredAt} */
    public async findByDeviceTriggerAndOccurredAt(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        occurredAt: IsoDateTime,
    ): Promise<DeviceReminderActionFact | undefined> {
        const row = await queryOne(
            this.executor,
            `SELECT * FROM im_reminder_action_facts
             WHERE device_id = $1 AND reminder_trigger_id = $2 AND occurred_at = $3
             LIMIT 1`,
            [deviceId, reminderTriggerId, occurredAt],
        );
        return row === undefined ? undefined : mapReminderActionFact(row);
    }

    /** {@inheritDoc ReminderActionFactRepository.findLatestByDeviceAndTrigger} */
    public async findLatestByDeviceAndTrigger(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
    ): Promise<DeviceReminderActionFact | undefined> {
        const row = await queryOne(
            this.executor,
            `SELECT * FROM im_reminder_action_facts
             WHERE device_id = $1 AND reminder_trigger_id = $2
             ORDER BY occurred_at DESC, received_at DESC, event_id DESC LIMIT 1`,
            [deviceId, reminderTriggerId],
        );
        return row === undefined ? undefined : mapReminderActionFact(row);
    }

    /** {@inheritDoc ReminderActionFactRepository.createIfAbsent} */
    public async createIfAbsent(
        fact: DeviceReminderActionFact,
    ): Promise<{ readonly fact: DeviceReminderActionFact; readonly created: boolean }> {
        const values = this.values(fact);
        const quoted = FACT_COLUMNS.map((column) => `"${column}"`).join(', ');
        const placeholders = FACT_COLUMNS.map((_, index) => `$${index + 1}`).join(', ');
        const inserted = await queryOne(
            this.executor,
            `INSERT INTO im_reminder_action_facts (${quoted}) VALUES (${placeholders})
             ON CONFLICT DO NOTHING RETURNING *`,
            values,
        );
        if (inserted !== undefined) return { fact: mapReminderActionFact(inserted), created: true };
        const existing = await this.findByEventId(fact.eventId);
        if (existing !== undefined) return { fact: existing, created: false };
        const operation = await this.findByDeviceAndOperationId(fact.report.deviceId, fact.report.operationId);
        if (operation !== undefined) return { fact: operation, created: false };
        const sameTime = await this.findByDeviceTriggerAndOccurredAt(
            fact.report.deviceId,
            fact.report.reminderTriggerId,
            fact.report.occurredAt,
        );
        if (sameTime !== undefined) return { fact: sameTime, created: false };
        throw new Error('reminder action fact conflict row vanished');
    }

    /** {@inheritDoc ReminderActionFactRepository.save} */
    public save(fact: DeviceReminderActionFact): Promise<void> {
        return upsert(this.executor, 'im_reminder_action_facts', FACT_COLUMNS, this.values(fact), ['event_id']);
    }

    private values(fact: DeviceReminderActionFact): readonly unknown[] {
        const report = fact.report;
        return [
            fact.eventId,
            fact.fingerprint,
            report.schemaVersion,
            report.correlationId,
            report.deviceId,
            report.reminderTriggerId,
            report.operationId,
            report.action,
            report.status,
            report.occurredAt,
            report.nextTriggerAt ?? null,
            report.errorCode ?? null,
            toJson(report.details),
            fact.receivedAt,
        ];
    }
}
