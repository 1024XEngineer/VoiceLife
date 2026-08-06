import type { ChannelAccountId, ExternalIdentityId } from '../../../contracts/ids.js';
import type { ExternalIdentity } from '../../../domain/models.js';
import type { IdentityRepository } from '../../../ports/repositories.js';
import { mapExternalIdentity } from './mappers.js';
import { queryOne, upsert, type SqlExecutor } from './sql.js';

const IDENTITY_COLUMNS = [
    'id',
    'channel_account_id',
    'external_user_id_ciphertext',
    'external_user_id_hash',
    'display_name',
    'status',
    'created_at',
    'updated_at',
] as const;

/** 受保护外部身份的 PostgreSQL 实现。 */
export class PostgresIdentityRepository implements IdentityRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc IdentityRepository.findById} */
    public async findById(id: ExternalIdentityId): Promise<ExternalIdentity | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_external_identities WHERE id = $1', [id]);
        return row === undefined ? undefined : mapExternalIdentity(row);
    }

    /** {@inheritDoc IdentityRepository.findByChannelAndHash} */
    public async findByChannelAndHash(
        channelAccountId: ChannelAccountId,
        externalUserIdHash: string,
    ): Promise<ExternalIdentity | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_external_identities WHERE channel_account_id = $1 AND external_user_id_hash = $2 LIMIT 1',
            [channelAccountId, externalUserIdHash],
        );
        return row === undefined ? undefined : mapExternalIdentity(row);
    }

    /** {@inheritDoc IdentityRepository.save} */
    public async save(identity: ExternalIdentity): Promise<void> {
        await upsert(
            this.executor,
            'im_external_identities',
            IDENTITY_COLUMNS,
            [
                identity.id,
                identity.channelAccountId,
                identity.externalUserIdCiphertext,
                identity.externalUserIdHash,
                identity.displayName ?? null,
                identity.status,
                identity.createdAt,
                identity.updatedAt,
            ],
            ['id'],
        );
    }
}
