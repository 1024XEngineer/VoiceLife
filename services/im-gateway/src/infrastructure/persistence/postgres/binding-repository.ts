import type { BindingId, DeviceId, ExternalIdentityId, UserId } from '../../../contracts/ids.js';
import type { ImBinding } from '../../../domain/models.js';
import type { BindingRepository } from '../../../ports/repositories.js';
import { mapBinding } from './mappers.js';
import { queryOne, upsert, type SqlExecutor } from './sql.js';

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
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

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
