import type { NormalizedImEvent } from '../contracts/platform-events.js';
import type { ReminderActionCommand } from '../contracts/device-gateway.js';
import { ImGatewayError } from '../shared/errors.js';
import type {
    ActionUiApplication,
    InboundEventApplication,
    PairingApplication,
    PlatformEventApplication,
    ReceiptApplication,
} from './api.js';

/** 将平台事件路由至绑定、回执或动作流程的默认实现。 */
export class DefaultPlatformEventApplication implements PlatformEventApplication {
    /**
     * 创建平台事件路由服务。
     * @param inboundEvents 入站事件状态服务。
     * @param pairing 配对服务。
     * @param receipts 回执服务。
     * @param actionUi 动作入口服务。
     */
    public constructor(
        private readonly inboundEvents: InboundEventApplication,
        private readonly pairing: PairingApplication,
        private readonly receipts: ReceiptApplication,
        private readonly actionUi: ActionUiApplication,
    ) {}

    /** {@inheritDoc PlatformEventApplication.postEvent} */
    public async postEvent(event: NormalizedImEvent): Promise<void | ReminderActionCommand> {
        if ((await this.inboundEvents.recordIfNew(event)) === 'duplicate') return;
        await this.inboundEvents.markProcessing(event.id);
        try {
            const result = await this.dispatch(event);
            await this.inboundEvents.markProcessed(event.id);
            return result;
        } catch (error) {
            await this.inboundEvents.markFailed(event.id);
            throw error;
        }
    }

    private async dispatch(event: NormalizedImEvent): Promise<void | ReminderActionCommand> {
        if (event.type === 'action.triggered') {
            return this.actionUi.execute(
                event.payload,
                event.externalIdentityId === undefined ? undefined : { actualIdentityId: event.externalIdentityId },
            );
        }

        if (event.type === 'binding.requested') {
            await this.pairing.confirm({
                ...event.payload,
                channelAccountId: event.channelAccountId,
            });
            return;
        }

        if (event.type === 'delivery.updated') {
            if (event.payload.channelAccountId !== event.channelAccountId) {
                throw new ImGatewayError(
                    'invalid_transition',
                    'Receipt channel does not match its normalized event envelope',
                );
            }
            await this.receipts.record(event.payload);
        }
    }
}
