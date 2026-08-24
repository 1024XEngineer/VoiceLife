import type { ActionId, DeviceId, OperationId, ReminderTriggerId } from '../../../contracts/ids.js';
import type { ImAction } from '../../../domain/models.js';
import type { ActionRepository } from '../../../ports/repositories.js';
import type { IsoDateTime } from '../../../shared/types.js';
import { mapAction } from './mappers.js';
import { queryOne, toJson, upsert, type SqlExecutor } from './sql.js';

const ACTION_COLUMNS = [
    'id',
    'operation_id',
    'correlation_id',
    'delivery_id',
    'actor_binding_id',
    'device_id',
    'reminder_trigger_id',
    'action_type',
    'action_params',
    'action_key_hash',
    'expected_identity_id',
    'actual_identity_id',
    'status',
    'dispatched_at',
    'result',
    'expires_at',
    'created_at',
    'updated_at',
] as const;

/** 提醒动作的 PostgreSQL 实现。 */
export class PostgresActionRepository implements ActionRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc ActionRepository.findById} */
    public async findById(id: ActionId): Promise<ImAction | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_actions WHERE id = $1', [id]);
        return row === undefined ? undefined : mapAction(row);
    }

    /** {@inheritDoc ActionRepository.findByOperationId} */
    public async findByOperationId(operationId: OperationId): Promise<ImAction | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_actions WHERE operation_id = $1 LIMIT 1', [
            operationId,
        ]);
        return row === undefined ? undefined : mapAction(row);
    }

    /** {@inheritDoc ActionRepository.findByResultOperationId} */
    public async findByResultOperationId(operationId: OperationId): Promise<ImAction | undefined> {
        const row = await queryOne(
            this.executor,
            "SELECT * FROM im_actions WHERE result->>'operationId' = $1 LIMIT 1",
            [operationId],
        );
        return row === undefined ? undefined : mapAction(row);
    }

    /** {@inheritDoc ActionRepository.findByActionKeyHash} */
    public async findByActionKeyHash(actionKeyHash: string): Promise<ImAction | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_actions WHERE action_key_hash = $1 LIMIT 1', [
            actionKeyHash,
        ]);
        return row === undefined ? undefined : mapAction(row);
    }

    /** {@inheritDoc ActionRepository.findPendingByDeviceAndTrigger} */
    public async findPendingByDeviceAndTrigger(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<readonly ImAction[]> {
        const { rows } = await this.executor.query(
            `SELECT * FROM im_actions
             WHERE device_id = $1 AND reminder_trigger_id = $2 AND expires_at > $3
               AND status IN ('pending', 'dispatched', 'processing')
             ORDER BY created_at ASC, id ASC`,
            [deviceId, reminderTriggerId, now],
        );
        return rows.map(mapAction);
    }

    /** {@inheritDoc ActionRepository.findExpiredActions} */
    public async findExpiredActions(now: IsoDateTime): Promise<readonly ImAction[]> {
        const { rows } = await this.executor.query(
            `SELECT * FROM im_actions
             WHERE expires_at <= $1 AND status IN ('pending', 'dispatched', 'processing')
             ORDER BY created_at ASC, id ASC`,
            [now],
        );
        return rows.map(mapAction);
    }

    /** {@inheritDoc ActionRepository.save} */
    public async save(action: ImAction): Promise<void> {
        await upsert(
            this.executor,
            'im_actions',
            ACTION_COLUMNS,
            [
                action.id,
                action.operationId,
                action.correlationId,
                action.deliveryId,
                action.actorBindingId,
                action.deviceId,
                action.reminderTriggerId,
                action.actionType,
                toJson(action.actionParams),
                action.actionKeyHash,
                action.expectedIdentityId,
                action.actualIdentityId ?? null,
                action.status,
                action.dispatchedAt ?? null,
                toJson(action.result),
                action.expiresAt,
                action.createdAt,
                action.updatedAt,
            ],
            ['id'],
        );
    }

    /** {@inheritDoc ActionRepository.createIfAbsent} */
    public async createIfAbsent(action: ImAction): Promise<{ readonly action: ImAction; readonly created: boolean }> {
        const quoted = ACTION_COLUMNS.map((column) => `"${column}"`).join(', ');
        const placeholders = ACTION_COLUMNS.map((_, index) => `$${index + 1}`).join(', ');
        const row = [
            action.id,
            action.operationId,
            action.correlationId,
            action.deliveryId,
            action.actorBindingId,
            action.deviceId,
            action.reminderTriggerId,
            action.actionType,
            toJson(action.actionParams),
            action.actionKeyHash,
            action.expectedIdentityId,
            action.actualIdentityId ?? null,
            action.status,
            action.dispatchedAt ?? null,
            toJson(action.result),
            action.expiresAt,
            action.createdAt,
            action.updatedAt,
        ];
        const inserted = await queryOne(
            this.executor,
            `INSERT INTO im_actions (${quoted}) VALUES (${placeholders}) ON CONFLICT DO NOTHING RETURNING *`,
            row,
        );
        if (inserted !== undefined) return { action: mapAction(inserted), created: true };
        const existing = (await this.findById(action.id)) ?? (await this.findByActionKeyHash(action.actionKeyHash));
        if (existing === undefined) throw new Error('im_actions conflict row vanished after idempotent insert');
        return { action: existing, created: false };
    }
}
