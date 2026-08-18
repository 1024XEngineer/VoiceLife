import type { BindingId, DeviceId, ExternalIdentityId, UserId } from '../../../contracts/ids.js';
import type { ImBinding } from '../../../domain/models.js';
import type { BindingRepository } from '../../../ports/repositories.js';
import { mapBinding } from './mappers.js';
import { queryOne, upsert, type SqlExecutor } from './sql.js';

/** 与 migration lock 不同的事务级串行化键。 */
export const BINDING_REPLACEMENT_LOCK_KEY = 727271001377;

const BINDING_COLUMNS = [
    'id',
    'user_id',
    'device_id',
    'external_identity_id',
    'priority',
    'status',
    'bound_at',
    'unbound_at',
    'revoked_at',
] as const;

/** 绑定关系的 PostgreSQL 实现。 */
export class PostgresBindingRepository implements BindingRepository {
    private replacementLocked = false;

    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc BindingRepository.acquireReplacementLock} */
    public async acquireReplacementLock(): Promise<void> {
        if (this.replacementLocked) return;
        await this.executor.query('SELECT pg_advisory_xact_lock($1)', [BINDING_REPLACEMENT_LOCK_KEY]);
        this.replacementLocked = true;
    }

    /** {@inheritDoc BindingRepository.createActiveIfAbsent} */
    public createActiveIfAbsent(binding: ImBinding): Promise<ImBinding> {
        return this.replaceActiveBinding(binding);
    }

    /** {@inheritDoc BindingRepository.replaceActiveBinding} */
    public async replaceActiveBinding(binding: ImBinding): Promise<ImBinding> {
        if (binding.status !== 'active' || binding.deviceId === undefined) {
            throw new Error('Active binding replacement requires an active binding with a device');
        }
        await this.acquireReplacementLock();
        const existing = await queryOne(
            this.executor,
            `SELECT * FROM im_bindings
             WHERE user_id = $1 AND device_id = $2 AND external_identity_id = $3 AND status = 'active'
             LIMIT 1`,
            [binding.userId, binding.deviceId, binding.externalIdentityId],
        );
        if (existing !== undefined) return mapBinding(existing);
        await this.executor.query(
            `UPDATE im_bindings SET status = 'unbound', unbound_at = $3
             WHERE status = 'active' AND (device_id = $1 OR external_identity_id = $2)`,
            [binding.deviceId, binding.externalIdentityId, binding.boundAt],
        );
        const row = await queryOne(
            this.executor,
            `INSERT INTO im_bindings (${BINDING_COLUMNS.join(', ')})
             VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9) RETURNING *`,
            [
                binding.id,
                binding.userId,
                binding.deviceId,
                binding.externalIdentityId,
                binding.priority,
                binding.status,
                binding.boundAt,
                binding.unboundAt ?? null,
                binding.revokedAt ?? null,
            ],
        );
        if (row === undefined) throw new Error('Binding insertion returned no row');
        return mapBinding(row);
    }

    /** {@inheritDoc BindingRepository.findById} */
    public async findById(id: BindingId): Promise<ImBinding | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_bindings WHERE id = $1', [id]);
        return row === undefined ? undefined : mapBinding(row);
    }

    /** {@inheritDoc BindingRepository.listActiveByUser} */
    public async listActiveByUser(userId: UserId): Promise<readonly ImBinding[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_bindings WHERE user_id = $1 AND status = $2 ORDER BY priority ASC',
            [userId, 'active'],
        );
        return rows.map(mapBinding);
    }

    /** {@inheritDoc BindingRepository.findActiveByDevice} */
    public async findActiveByDevice(deviceId: DeviceId): Promise<readonly ImBinding[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_bindings WHERE device_id = $1 AND status = $2 ORDER BY priority ASC',
            [deviceId, 'active'],
        );
        return rows.map(mapBinding);
    }

    /** {@inheritDoc BindingRepository.findActiveByIdentity} */
    public async findActiveByIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_bindings WHERE external_identity_id = $1 AND status = $2 ORDER BY priority ASC LIMIT 1',
            [externalIdentityId, 'active'],
        );
        return row === undefined ? undefined : mapBinding(row);
    }

    /** {@inheritDoc BindingRepository.save} */
    public async save(binding: ImBinding): Promise<void> {
        await upsert(
            this.executor,
            'im_bindings',
            BINDING_COLUMNS,
            [
                binding.id,
                binding.userId,
                binding.deviceId ?? null,
                binding.externalIdentityId,
                binding.priority,
                binding.status,
                binding.boundAt,
                binding.unboundAt ?? null,
                binding.revokedAt ?? null,
            ],
            ['id'],
        );
    }
}
