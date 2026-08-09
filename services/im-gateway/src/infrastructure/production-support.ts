import type { ChannelAccount, ExternalIdentity } from '../domain/models.js';
import type {
    ChannelCapabilityResolver,
    ChannelHealth,
    ChannelHealthPort,
    Clock,
    ConversationResolverPort,
} from '../ports/external.js';
import type { IsoDateTime } from '../shared/types.js';

/** 使用系统 UTC 时钟的生产实现。 */
export class SystemClock implements Clock {
    /** {@inheritDoc Clock.now} */
    public now(): IsoDateTime {
        return new Date().toISOString() as IsoDateTime;
    }

    /** {@inheritDoc Clock.addMinutes} */
    public addMinutes(value: IsoDateTime, minutes: number): IsoDateTime {
        return new Date(Date.parse(value) + minutes * 60_000).toISOString() as IsoDateTime;
    }
}

/** 按账号状态和当前平台能力生成生产健康结果。 */
export class CapabilityChannelHealthPort implements ChannelHealthPort {
    /**
     * @param capabilities 平台能力解析器。
     * @param clock 健康检查时间来源。
     */
    public constructor(
        private readonly capabilities: ChannelCapabilityResolver,
        private readonly clock: Clock,
    ) {}

    /** {@inheritDoc ChannelHealthPort.check} */
    public async check(account: ChannelAccount): Promise<ChannelHealth> {
        if (account.status !== 'active') {
            return { accountId: account.id, status: 'unavailable', checkedAt: this.clock.now() };
        }
        const available = await this.capabilities.resolve(account);
        return {
            accountId: account.id,
            status: available.proactiveMessage ? 'healthy' : 'degraded',
            checkedAt: this.clock.now(),
            ...(available.proactiveMessage ? {} : { detail: 'proactive_message_unavailable' }),
        };
    }
}

/** 将受保护外部身份直接映射为同一账号下的私聊会话。 */
export class DirectConversationResolver implements ConversationResolverPort {
    /** {@inheritDoc ConversationResolverPort.resolveDirect} */
    public resolveDirect(identity: ExternalIdentity): Promise<{
        readonly channelAccountId: ExternalIdentity['channelAccountId'];
        readonly externalIdentityId: ExternalIdentity['id'];
        readonly kind: 'direct';
        readonly externalConversationIdCiphertext: string;
    }> {
        return Promise.resolve({
            channelAccountId: identity.channelAccountId,
            externalIdentityId: identity.id,
            kind: 'direct',
            externalConversationIdCiphertext: identity.externalUserIdCiphertext,
        });
    }
}
