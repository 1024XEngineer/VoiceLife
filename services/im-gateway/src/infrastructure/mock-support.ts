import type {
    ActionId,
    BindingId,
    ChannelAccountId,
    DeliveryAttemptId,
    DeliveryId,
    DeliveryReceiptId,
    DeviceId,
    ExternalIdentityId,
    OperationId,
    OutboxEventId,
    PairingSessionId,
    RequestId,
    UserId,
} from '../contracts/ids.js';
import { unsafeId } from '../contracts/ids.js';
import type { ReminderActionCommand } from '../contracts/device-gateway.js';
import type {
    ActionCommandStreamPort,
    ActionStreamCloseScope,
    ActionStreamSubscription,
    ActionTokenClaims,
    ActionTokenPort,
    ChannelCapabilityResolver,
    ChannelHealth,
    ChannelHealthPort,
    Clock,
    ConversationResolverPort,
    DeliveryRendererPort,
    DeviceAuthenticationPort,
    DevicePrincipal,
    ExternalIdentityProtector,
    IdGenerator,
    ImChannelPort,
    ImSendAcceptance,
    OutboundImMessage,
    PairingCodePort,
    ProtectedExternalIdentity,
} from '../ports/external.js';
import type {
    ChannelAccount,
    ChannelCapabilities,
    ConversationRef,
    Delivery,
    ExternalIdentity,
} from '../domain/models.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';
import { ImGatewayError } from '../shared/errors.js';

/** 可手动推进时间的确定性测试时钟。 */
export class FixedClock implements Clock {
    /** @param value 初始日期时间。 */
    public constructor(private value: IsoDateTime = '2026-08-03T00:00:00.000Z' as IsoDateTime) {}

    /** {@inheritDoc Clock.now} */
    public now(): IsoDateTime {
        return this.value;
    }

    /** {@inheritDoc Clock.addMinutes} */
    public addMinutes(value: IsoDateTime, minutes: number): IsoDateTime {
        return new Date(Date.parse(value) + minutes * 60_000).toISOString() as IsoDateTime;
    }

    /**
     * 将测试时钟向前推进指定分钟数。
     * @param minutes 要推进的分钟数。
     */
    public advanceMinutes(minutes: number): void {
        this.value = this.addMinutes(this.value, minutes);
    }
}

/** 使用递增序列生成可预测测试标识的实现。 */
export class SequentialIdGenerator implements IdGenerator {
    private sequence = 0;

    /** {@inheritDoc IdGenerator.nextChannelAccountId} */
    public nextChannelAccountId(): ChannelAccountId {
        return this.next<ChannelAccountId>('channel');
    }
    /** {@inheritDoc IdGenerator.nextPairingSessionId} */
    public nextPairingSessionId(): PairingSessionId {
        return this.next<PairingSessionId>('pairing');
    }
    /** {@inheritDoc IdGenerator.nextBindingId} */
    public nextBindingId(): BindingId {
        return this.next<BindingId>('binding');
    }
    /** {@inheritDoc IdGenerator.nextExternalIdentityId} */
    public nextExternalIdentityId(): ExternalIdentityId {
        return this.next<ExternalIdentityId>('identity');
    }
    /** {@inheritDoc IdGenerator.nextDeliveryId} */
    public nextDeliveryId(): DeliveryId {
        return this.next<DeliveryId>('delivery');
    }
    /** {@inheritDoc IdGenerator.nextDeliveryAttemptId} */
    public nextDeliveryAttemptId(): DeliveryAttemptId {
        return this.next<DeliveryAttemptId>('attempt');
    }
    /** {@inheritDoc IdGenerator.nextDeliveryReceiptId} */
    public nextDeliveryReceiptId(): DeliveryReceiptId {
        return this.next<DeliveryReceiptId>('receipt');
    }
    /** {@inheritDoc IdGenerator.actionIdForDelivery} */
    public actionIdForDelivery(deliveryId: DeliveryId): ActionId {
        return unsafeId<ActionId>(`action-ui:${deliveryId}`);
    }
    /** {@inheritDoc IdGenerator.nextOperationId} */
    public nextOperationId(): OperationId {
        return this.next<OperationId>('operation');
    }
    /** {@inheritDoc IdGenerator.nextOutboxEventId} */
    public nextOutboxEventId(): OutboxEventId {
        return this.next<OutboxEventId>('outbox');
    }
    /** {@inheritDoc IdGenerator.nextRequestId} */
    public nextRequestId(): RequestId {
        return this.next<RequestId>('request');
    }

    private next<T>(prefix: string): T {
        this.sequence += 1;
        return unsafeId<T>(`${prefix}-${this.sequence}`);
    }
}

/** 固定签发六位展示码的配对码测试实现。 */
export class MockPairingCodePort implements PairingCodePort {
    /** {@inheritDoc PairingCodePort.issue} */
    public issue(): Promise<{ readonly displayCode: string; readonly hash: string }> {
        return Promise.resolve({ displayCode: '123456', hash: 'hash:123456' });
    }

    /** {@inheritDoc PairingCodePort.hash} */
    public hash(displayCode: string): Promise<string> {
        return Promise.resolve(`hash:${displayCode}`);
    }
}

/** 使用可预测前缀模拟外部身份保护的测试实现。 */
export class MockExternalIdentityProtector implements ExternalIdentityProtector {
    /** {@inheritDoc ExternalIdentityProtector.protect} */
    public protect(plainExternalUserId: string): Promise<ProtectedExternalIdentity> {
        return Promise.resolve({
            ciphertext: `ciphertext:${plainExternalUserId}`,
            hash: `hash:${plainExternalUserId}`,
        });
    }
}

/** 返回固定渠道能力集合的测试实现。 */
export class MockChannelCapabilities implements ChannelCapabilityResolver {
    /** {@inheritDoc ChannelCapabilityResolver.resolve} */
    public resolve(_account: ChannelAccount): Promise<ChannelCapabilities> {
        return Promise.resolve({
            proactiveMessage: true,
            nativeAction: false,
            actionUi: true,
            deliveryReceipt: true,
            presentationTypes: ['template', 'text_with_action_ui'],
        });
    }
}

/** 根据账号状态生成健康结果的测试实现。 */
export class MockChannelHealthPort implements ChannelHealthPort {
    /** @param clock 用于记录检查时间的时钟。 */
    public constructor(private readonly clock: Clock) {}

    /** {@inheritDoc ChannelHealthPort.check} */
    public check(account: ChannelAccount): Promise<ChannelHealth> {
        return Promise.resolve({
            accountId: account.id,
            status: account.status === 'active' ? 'healthy' : 'unavailable',
            checkedAt: this.clock.now(),
        });
    }
}

/** 从外部身份生成确定性私聊引用的测试实现。 */
export class MockConversationResolver implements ConversationResolverPort {
    /** {@inheritDoc ConversationResolverPort.resolveDirect} */
    public resolveDirect(identity: ExternalIdentity): Promise<ConversationRef> {
        return Promise.resolve({
            channelAccountId: identity.channelAccountId,
            externalIdentityId: identity.id,
            kind: 'direct',
            externalConversationIdCiphertext: identity.externalUserIdCiphertext,
        });
    }
}

/** 将语义载荷和可选动作令牌组合成测试消息的渲染器。 */
export class MockDeliveryRenderer implements DeliveryRendererPort {
    /** {@inheritDoc DeliveryRendererPort.render} */
    public render(
        delivery: Delivery,
        _account: ChannelAccount,
        _capabilities: ChannelCapabilities,
        context: { readonly actionToken?: string },
    ): Promise<JsonValue> {
        if (context.actionToken === undefined) {
            return Promise.resolve(delivery.semanticPayload);
        }
        return Promise.resolve({
            semanticPayload: delivery.semanticPayload,
            actionUi: { token: context.actionToken },
        });
    }
}

/** 始终接受发送请求的 IM 渠道测试实现。 */
export class MockImChannel implements ImChannelPort {
    /** {@inheritDoc ImChannelPort.send} */
    public send(_message: OutboundImMessage): Promise<ImSendAcceptance> {
        return Promise.resolve({
            accepted: true,
            platformMessageId: 'mock-platform-message',
        });
    }
}

/** 仅提供快照回放的测试命令流，不模拟实时 SSE Hub。 */
export class InMemoryActionCommandStream implements ActionCommandStreamPort {
    private readonly commands: ReminderActionCommand[] = [];

    /** {@inheritDoc ActionCommandStreamPort.publish} */
    public publish(command: ReminderActionCommand): Promise<void> {
        this.commands.push(command);
        return Promise.resolve();
    }

    /** {@inheritDoc ActionCommandStreamPort.subscribe} */
    public subscribe(subscription: ActionStreamSubscription): AsyncIterable<ReminderActionCommand> {
        const matching = this.commands.filter(
            (command) =>
                command.deviceId === subscription.deviceId &&
                command.reminderTriggerId === subscription.reminderTriggerId &&
                command.expiresAt <= subscription.expiresAt,
        );
        const start =
            subscription.lastEventId === undefined
                ? 0
                : Math.max(0, matching.findIndex((command) => command.commandId === subscription.lastEventId) + 1);
        return snapshot(matching.slice(start));
    }

    /** {@inheritDoc ActionCommandStreamPort.close} */
    public close(actionId: ActionId, _scope: ActionStreamCloseScope): Promise<void> {
        for (let index = this.commands.length - 1; index >= 0; index -= 1) {
            if (this.commands[index]?.commandId === actionId) this.commands.splice(index, 1);
        }
        return Promise.resolve();
    }
}

/** 在内存中保存动作令牌声明的测试实现。 */
export class InMemoryActionTokenPort implements ActionTokenPort {
    private readonly claimsByToken = new Map<string, ActionTokenClaims>();

    /** {@inheritDoc ActionTokenPort.issue} */
    public issue(claims: ActionTokenClaims): Promise<string> {
        const token = `mock-token:${claims.actionId}`;
        this.claimsByToken.set(token, claims);
        return Promise.resolve(token);
    }

    /** {@inheritDoc ActionTokenPort.verify} */
    public verify(token: string): Promise<ActionTokenClaims> {
        const claims = this.claimsByToken.get(token);
        if (claims === undefined) {
            return Promise.reject(new ImGatewayError('action_not_found', 'Action token is invalid'));
        }
        return Promise.resolve(claims);
    }

    /** {@inheritDoc ActionTokenPort.fingerprint} */
    public fingerprint(token: string): Promise<string> {
        return Promise.resolve(`hash:${token}`);
    }
}

/** 始终认证为指定设备的测试认证端口。 */
export class MockDeviceAuthenticationPort implements DeviceAuthenticationPort {
    /**
     * @param deviceId 所有认证请求返回的设备标识。
     * @param userId 与测试设备凭据绑定的用户标识。
     */
    public constructor(
        private readonly deviceId: DeviceId,
        private readonly userId: UserId = unsafeId<UserId>('user-fixture'),
    ) {}

    /** @returns 测试 fixture 可用于预置匹配设备记录的可信主体。 */
    public get principal(): DevicePrincipal {
        return { deviceId: this.deviceId, userId: this.userId };
    }

    /** {@inheritDoc DeviceAuthenticationPort.authenticate} */
    public authenticate(_authorization: string): Promise<DevicePrincipal> {
        return Promise.resolve(this.principal);
    }
}

async function* snapshot(commands: readonly ReminderActionCommand[]): AsyncIterable<ReminderActionCommand> {
    for (const command of commands) yield command;
}
