import type { ChannelAccountId } from '../contracts/ids.js';
import type { ChannelAccount, ChannelCapabilities, Delivery } from '../domain/models.js';
import type {
    ChannelCapabilityResolver,
    DeliveryRendererPort,
    ImChannelPort,
    ImSendAcceptance,
    OutboundImMessage,
} from '../ports/external.js';
import { ImGatewayError } from '../shared/errors.js';
import type { JsonValue } from '../shared/types.js';

/** 单个渠道账号可用于能力解析、投递渲染和消息发送的适配器集合。 */
export interface ChannelAdapter extends ChannelCapabilityResolver, DeliveryRendererPort, ImChannelPort {}

/** 渠道账号与其平台适配器的显式注册关系。 */
export interface ChannelAdapterRegistration {
    readonly accountId: ChannelAccountId;
    readonly adapter: ChannelAdapter;
}

/** 按渠道账号分发能力、渲染和发送调用的多渠道适配器注册表。 */
export class ChannelAdapterRegistry implements ChannelCapabilityResolver, DeliveryRendererPort, ImChannelPort {
    private readonly adapters: ReadonlyMap<ChannelAccountId, ChannelAdapter>;

    /**
     * @param registrations 渠道账号与适配器的一一对应关系。
     */
    public constructor(registrations: readonly ChannelAdapterRegistration[]) {
        const adapters = new Map<ChannelAccountId, ChannelAdapter>();
        for (const registration of registrations) {
            if (registration.accountId.trim() === '') {
                throw new ImGatewayError('invalid_contract', 'Channel adapter registration requires an account ID');
            }
            if (adapters.has(registration.accountId)) {
                throw new ImGatewayError(
                    'invalid_contract',
                    `Channel adapter is already registered for ${registration.accountId}`,
                );
            }
            adapters.set(registration.accountId, registration.adapter);
        }
        this.adapters = adapters;
    }

    /** {@inheritDoc ChannelCapabilityResolver.resolve} */
    public async resolve(account: ChannelAccount): Promise<ChannelCapabilities> {
        return this.adapterFor(account.id).resolve(account);
    }

    /** {@inheritDoc DeliveryRendererPort.render} */
    public async render(
        delivery: Delivery,
        account: ChannelAccount,
        capabilities: ChannelCapabilities,
        context: { readonly actionToken?: string },
    ): Promise<JsonValue> {
        return this.adapterFor(account.id).render(delivery, account, capabilities, context);
    }

    /** {@inheritDoc ImChannelPort.send} */
    public async send(message: OutboundImMessage): Promise<ImSendAcceptance> {
        return this.adapterFor(message.delivery.channelAccountId).send(message);
    }

    private adapterFor(accountId: ChannelAccountId): ChannelAdapter {
        const adapter = this.adapters.get(accountId);
        if (adapter === undefined) {
            throw new ImGatewayError('invalid_contract', `No channel adapter is registered for ${accountId}`);
        }
        return adapter;
    }
}
