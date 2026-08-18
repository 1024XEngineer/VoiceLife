import type { ChannelAccountId } from '../contracts/ids.js';
import type { ChannelAccount } from '../domain/models.js';
import type { ChannelHealth, ChannelHealthPort, Clock, IdGenerator } from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';
import { ImGatewayError } from '../shared/errors.js';
import type { ChannelAccountApplication, RegisterChannelAccountCommand } from './api.js';

/** 渠道账号注册、停用、查询与健康检查的默认实现。 */
export class DefaultChannelAccountApplication implements ChannelAccountApplication {
    /**
     * 创建渠道账号应用服务。
     * @param unitOfWork 事务工作单元。
     * @param ids 标识生成器。
     * @param clock 业务时钟。
     * @param healthPort 渠道健康检查端口。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
        private readonly healthPort: ChannelHealthPort,
    ) {}

    /** {@inheritDoc ChannelAccountApplication.register} */
    public register(command: RegisterChannelAccountCommand): Promise<ChannelAccount> {
        return this.unitOfWork.transaction(async (tx) => {
            const now = this.clock.now();
            const account: ChannelAccount = {
                id: this.ids.nextChannelAccountId(),
                platform: command.platform,
                tenantExternalId: command.tenantExternalId,
                koishiBotId: command.koishiBotId,
                credentialRef: command.credentialRef,
                connectionMode: command.connectionMode,
                ...(command.capabilityConfig === undefined ? {} : { capabilityConfig: command.capabilityConfig }),
                status: 'active',
                createdAt: now,
                updatedAt: now,
            };
            await tx.channelAccounts.save(account);
            return account;
        });
    }

    /** {@inheritDoc ChannelAccountApplication.disable} */
    public disable(channelAccountId: ChannelAccountId): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            const account = await tx.channelAccounts.findById(channelAccountId);
            if (account === undefined) return;
            await tx.channelAccounts.save({
                ...account,
                status: 'disabled',
                updatedAt: this.clock.now(),
            });
        });
    }

    /** {@inheritDoc ChannelAccountApplication.find} */
    public find(channelAccountId: ChannelAccountId): Promise<ChannelAccount | undefined> {
        return this.unitOfWork.transaction((tx) => tx.channelAccounts.findById(channelAccountId));
    }

    /** {@inheritDoc ChannelAccountApplication.health} */
    public async health(channelAccountId: ChannelAccountId): Promise<ChannelHealth> {
        const account = await this.find(channelAccountId);
        if (account === undefined) {
            throw new ImGatewayError('binding_not_found', 'Channel account was not found');
        }
        return this.healthPort.check(account);
    }
}
