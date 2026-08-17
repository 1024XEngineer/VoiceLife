import type { DeviceId, UserId } from '../../../contracts/ids.js';
import type { ImDevice } from '../../../domain/models.js';
import type { DeviceRepository } from '../../../ports/repositories.js';
import { mapDevice } from './mappers.js';
import { queryOne, type SqlExecutor } from './sql.js';

const DEVICE_COLUMNS = ['device_id', 'user_id', 'token_digest', 'status', 'created_at', 'updated_at'] as const;

/** 注册设备的 PostgreSQL 实现。 */
export class PostgresDeviceRepository implements DeviceRepository {
    /** @param executor 当前事务执行器。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc DeviceRepository.create} */
    public async create(device: ImDevice): Promise<void> {
        await this.executor.query(
            `INSERT INTO im_devices (${DEVICE_COLUMNS.join(', ')}) VALUES ($1, $2, $3, $4, $5, $6)`,
            [
                device.deviceId,
                device.userId,
                Buffer.from(device.tokenDigest),
                device.status,
                device.createdAt,
                device.updatedAt,
            ],
        );
    }

    /** {@inheritDoc DeviceRepository.findById} */
    public async findById(deviceId: DeviceId): Promise<ImDevice | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_devices WHERE device_id = $1', [deviceId]);
        return row === undefined ? undefined : mapDevice(row);
    }

    /** {@inheritDoc DeviceRepository.findByTokenDigest} */
    public async findByTokenDigest(tokenDigest: Uint8Array): Promise<ImDevice | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_devices WHERE token_digest = $1', [
            Buffer.from(tokenDigest),
        ]);
        return row === undefined ? undefined : mapDevice(row);
    }

    /** {@inheritDoc DeviceRepository.lockById} */
    public async lockById(deviceId: DeviceId): Promise<ImDevice | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_devices WHERE device_id = $1 FOR UPDATE', [
            deviceId,
        ]);
        return row === undefined ? undefined : mapDevice(row);
    }

    /** {@inheritDoc DeviceRepository.list} */
    public async list(userId?: UserId): Promise<readonly ImDevice[]> {
        const result =
            userId === undefined
                ? await this.executor.query('SELECT * FROM im_devices ORDER BY created_at, device_id')
                : await this.executor.query(
                      'SELECT * FROM im_devices WHERE user_id = $1 ORDER BY created_at, device_id',
                      [userId],
                  );
        return result.rows.map(mapDevice);
    }

    /** {@inheritDoc DeviceRepository.save} */
    public async save(device: ImDevice): Promise<void> {
        await this.executor.query(
            `INSERT INTO im_devices (${DEVICE_COLUMNS.join(', ')}) VALUES ($1, $2, $3, $4, $5, $6)
             ON CONFLICT (device_id) DO UPDATE SET
                token_digest = EXCLUDED.token_digest,
                status = EXCLUDED.status,
                updated_at = EXCLUDED.updated_at`,
            [
                device.deviceId,
                device.userId,
                Buffer.from(device.tokenDigest),
                device.status,
                device.createdAt,
                device.updatedAt,
            ],
        );
    }
}
