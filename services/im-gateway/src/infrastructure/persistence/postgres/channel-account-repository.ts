import type { ChannelAccountId } from '../../../contracts/ids.js';
import type { ChannelAccount } from '../../../domain/models.js';
import type { ChannelAccountRepository } from '../../../ports/repositories.js';
import { mapChannelAccount } from './mappers.js';
import { queryOne, toJson, upsert, type SqlExecutor } from './sql.js';

const CHANNEL_COLUMNS = [
    'id',
    'platform',
    'tenant_external_id',
    'koishi_bot_id',
    'credential_ref',
    'connection_mode',
    'capability_config',
    'status',
    'created_at',
    'updated_at',
] as const;

/** 渠道账号的 PostgreSQL 实现。 */
export class PostgresChannelAccountRepository implements ChannelAccountRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc ChannelAccountRepository.findById} */
    public async findById(id: ChannelAccountId): Promise<ChannelAccount | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_channel_accounts WHERE id = $1', [id]);
        return row === undefined ? undefined : mapChannelAccount(row);
    }

    /** {@inheritDoc ChannelAccountRepository.save} */
    public async save(account: ChannelAccount): Promise<void> {
        await upsert(
            this.executor,
            'im_channel_accounts',
            CHANNEL_COLUMNS,
            [
                account.id,
                account.platform,
                account.tenantExternalId,
                account.koishiBotId,
                account.credentialRef,
                account.connectionMode,
                toJson(account.capabilityConfig),
                account.status,
                account.createdAt,
                account.updatedAt,
            ],
            ['id'],
        );
    }
}
