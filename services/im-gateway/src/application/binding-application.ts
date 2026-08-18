import type { BindingId, ExternalIdentityId, UserId } from '../contracts/ids.js';
import type { ImBinding } from '../domain/models.js';
import type { Clock } from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';
import { ImGatewayError } from '../shared/errors.js';
import type { BindingApplication } from './api.js';

/** 外部身份绑定查询、解绑与撤销的默认实现。 */
export class DefaultBindingApplication implements BindingApplication {
    /**
     * 创建绑定应用服务。
     * @param unitOfWork 事务工作单元。
     * @param clock 业务时钟。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly clock: Clock,
    ) {}

    /** {@inheritDoc BindingApplication.list} */
    public list(userId: UserId): Promise<readonly ImBinding[]> {
        return this.unitOfWork.transaction((tx) => tx.bindings.listActiveByUser(userId));
    }

    /** {@inheritDoc BindingApplication.unbind} */
    public unbind(bindingId: BindingId): Promise<void> {
        return this.changeStatus(bindingId, 'unbound');
    }

    /** {@inheritDoc BindingApplication.revoke} */
    public revoke(bindingId: BindingId): Promise<void> {
        return this.changeStatus(bindingId, 'revoked');
    }

    /** {@inheritDoc BindingApplication.findActiveByExternalIdentity} */
    public findActiveByExternalIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined> {
        return this.unitOfWork.transaction((tx) => tx.bindings.findActiveByIdentity(externalIdentityId));
    }

    private changeStatus(bindingId: BindingId, status: 'unbound' | 'revoked'): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            const binding = await tx.bindings.findById(bindingId);
            if (binding === undefined) {
                throw new ImGatewayError('binding_not_found', 'Binding was not found');
            }
            if (binding.status === status) return;
            if (binding.status !== 'active') {
                throw new ImGatewayError('invalid_transition', 'Binding is already in a terminal state');
            }
            const now = this.clock.now();
            await tx.bindings.save({
                ...binding,
                status,
                ...(status === 'unbound' ? { unboundAt: now } : { revokedAt: now }),
            });
        });
    }
}
