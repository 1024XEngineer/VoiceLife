import type { DeviceId, PairingSessionId } from '../../../contracts/ids.js';
import type { PairingSession } from '../../../domain/models.js';
import type { PairingSessionRepository } from '../../../ports/repositories.js';
import type { IsoDateTime } from '../../../shared/types.js';
import { mapPairingSession } from './mappers.js';
import { queryOne, toJson, upsert, type SqlExecutor } from './sql.js';

const PAIRING_COLUMNS = [
    'id',
    'display_code_hash',
    'user_id',
    'device_id',
    'allowed_platforms',
    'status',
    'expires_at',
    'created_at',
    'confirmed_at',
] as const;

/** 配对会话的 PostgreSQL 实现。 */
export class PostgresPairingSessionRepository implements PairingSessionRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc PairingSessionRepository.createPendingIfAbsent} */
    public async createPendingIfAbsent(session: PairingSession): Promise<boolean> {
        const row = await queryOne(
            this.executor,
            `INSERT INTO im_pairing_sessions (${PAIRING_COLUMNS.join(', ')})
             VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
             ON CONFLICT DO NOTHING
             RETURNING id`,
            [
                session.id,
                session.displayCodeHash,
                session.userId ?? null,
                session.deviceId,
                toJson(session.allowedPlatforms),
                session.status,
                session.expiresAt,
                session.createdAt,
                session.confirmedAt ?? null,
            ],
        );
        return row !== undefined;
    }

    /** {@inheritDoc PairingSessionRepository.cancelPendingByDevice} */
    public async cancelPendingByDevice(deviceId: DeviceId): Promise<void> {
        await this.executor.query('UPDATE im_pairing_sessions SET status = $1 WHERE device_id = $2 AND status = $3', [
            'cancelled',
            deviceId,
            'pending',
        ]);
    }

    /** {@inheritDoc PairingSessionRepository.findById} */
    public async findById(id: PairingSessionId): Promise<PairingSession | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_pairing_sessions WHERE id = $1', [id]);
        return row === undefined ? undefined : mapPairingSession(row);
    }

    /** {@inheritDoc PairingSessionRepository.findPendingByDisplayCodeHash} */
    public async findPendingByDisplayCodeHash(hash: string): Promise<PairingSession | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_pairing_sessions WHERE display_code_hash = $1 AND status = $2 ORDER BY created_at ASC, id ASC LIMIT 1',
            [hash, 'pending'],
        );
        return row === undefined ? undefined : mapPairingSession(row);
    }

    /** {@inheritDoc PairingSessionRepository.lockPendingByDisplayCodeHash} */
    public async lockPendingByDisplayCodeHash(hash: string): Promise<PairingSession | undefined> {
        const row = await queryOne(
            this.executor,
            `SELECT * FROM im_pairing_sessions
             WHERE display_code_hash = $1 AND status = $2
             FOR UPDATE`,
            [hash, 'pending'],
        );
        return row === undefined ? undefined : mapPairingSession(row);
    }

    /** {@inheritDoc PairingSessionRepository.lockPendingByIdAndDisplayCodeHash} */
    public async lockPendingByIdAndDisplayCodeHash(
        id: PairingSessionId,
        hash: string,
    ): Promise<PairingSession | undefined> {
        const row = await queryOne(
            this.executor,
            `SELECT * FROM im_pairing_sessions
             WHERE id = $1 AND display_code_hash = $2 AND status = $3
             FOR UPDATE`,
            [id, hash, 'pending'],
        );
        return row === undefined ? undefined : mapPairingSession(row);
    }

    /** {@inheritDoc PairingSessionRepository.transitionPending} */
    public async transitionPending(id: PairingSessionId, status: 'cancelled' | 'expired'): Promise<boolean> {
        const row = await queryOne(
            this.executor,
            `UPDATE im_pairing_sessions SET status = $2
             WHERE id = $1 AND status = 'pending'
             RETURNING id`,
            [id, status],
        );
        return row !== undefined;
    }

    /** {@inheritDoc PairingSessionRepository.findExpiredPairingSessions} */
    public async findExpiredPairingSessions(now: IsoDateTime): Promise<readonly PairingSession[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_pairing_sessions WHERE status = $1 AND expires_at <= $2 ORDER BY created_at ASC, id ASC',
            ['pending', now],
        );
        return rows.map(mapPairingSession);
    }

    /** {@inheritDoc PairingSessionRepository.save} */
    public async save(session: PairingSession): Promise<void> {
        await upsert(
            this.executor,
            'im_pairing_sessions',
            PAIRING_COLUMNS,
            [
                session.id,
                session.displayCodeHash,
                session.userId ?? null,
                session.deviceId,
                toJson(session.allowedPlatforms),
                session.status,
                session.expiresAt,
                session.createdAt,
                session.confirmedAt ?? null,
            ],
            ['id'],
        );
    }
}
