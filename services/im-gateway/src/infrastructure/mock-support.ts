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
} from '../contracts/ids.js';
import { unsafeId } from '../contracts/ids.js';
import type { ReminderActionCommand } from '../contracts/device-gateway.js';
import type {
    ActionCommandStreamPort,
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

export class FixedClock implements Clock {
    public constructor(private value: IsoDateTime = '2026-08-03T00:00:00.000Z' as IsoDateTime) {}

    public now(): IsoDateTime {
        return this.value;
    }

    public addMinutes(value: IsoDateTime, minutes: number): IsoDateTime {
        return new Date(Date.parse(value) + minutes * 60_000).toISOString() as IsoDateTime;
    }

    public advanceMinutes(minutes: number): void {
        this.value = this.addMinutes(this.value, minutes);
    }
}

export class SequentialIdGenerator implements IdGenerator {
    private sequence = 0;

    public nextChannelAccountId(): ChannelAccountId {
        return this.next<ChannelAccountId>('channel');
    }
    public nextPairingSessionId(): PairingSessionId {
        return this.next<PairingSessionId>('pairing');
    }
    public nextBindingId(): BindingId {
        return this.next<BindingId>('binding');
    }
    public nextExternalIdentityId(): ExternalIdentityId {
        return this.next<ExternalIdentityId>('identity');
    }
    public nextDeliveryId(): DeliveryId {
        return this.next<DeliveryId>('delivery');
    }
    public nextDeliveryAttemptId(): DeliveryAttemptId {
        return this.next<DeliveryAttemptId>('attempt');
    }
    public nextDeliveryReceiptId(): DeliveryReceiptId {
        return this.next<DeliveryReceiptId>('receipt');
    }
    public actionIdForDelivery(deliveryId: DeliveryId): ActionId {
        return unsafeId<ActionId>(`action-ui:${deliveryId}`);
    }
    public nextOperationId(): OperationId {
        return this.next<OperationId>('operation');
    }
    public nextOutboxEventId(): OutboxEventId {
        return this.next<OutboxEventId>('outbox');
    }
    public nextRequestId(): RequestId {
        return this.next<RequestId>('request');
    }

    private next<T>(prefix: string): T {
        this.sequence += 1;
        return unsafeId<T>(`${prefix}-${this.sequence}`);
    }
}

export class MockPairingCodePort implements PairingCodePort {
    public issue(): Promise<{ readonly displayCode: string; readonly hash: string }> {
        return Promise.resolve({ displayCode: '123456', hash: 'hash:123456' });
    }

    public hash(displayCode: string): Promise<string> {
        return Promise.resolve(`hash:${displayCode}`);
    }
}

export class MockExternalIdentityProtector implements ExternalIdentityProtector {
    public protect(plainExternalUserId: string): Promise<ProtectedExternalIdentity> {
        return Promise.resolve({
            ciphertext: `ciphertext:${plainExternalUserId}`,
            hash: `hash:${plainExternalUserId}`,
        });
    }
}

export class MockChannelCapabilities implements ChannelCapabilityResolver {
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

export class MockChannelHealthPort implements ChannelHealthPort {
    public constructor(private readonly clock: Clock) {}

    public check(account: ChannelAccount): Promise<ChannelHealth> {
        return Promise.resolve({
            accountId: account.id,
            status: account.status === 'active' ? 'healthy' : 'unavailable',
            checkedAt: this.clock.now(),
        });
    }
}

export class MockConversationResolver implements ConversationResolverPort {
    public resolveDirect(identity: ExternalIdentity): Promise<ConversationRef> {
        return Promise.resolve({
            channelAccountId: identity.channelAccountId,
            externalIdentityId: identity.id,
            kind: 'direct',
            externalConversationIdCiphertext: identity.externalUserIdCiphertext,
        });
    }
}

export class MockDeliveryRenderer implements DeliveryRendererPort {
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

export class MockImChannel implements ImChannelPort {
    public send(_message: OutboundImMessage): Promise<ImSendAcceptance> {
        return Promise.resolve({
            accepted: true,
            platformMessageId: 'mock-platform-message',
        });
    }
}

/** Snapshot-only stream used by skeleton tests; it is not a live SSE hub. */
export class InMemoryActionCommandStream implements ActionCommandStreamPort {
    private readonly commands: ReminderActionCommand[] = [];

    public publish(command: ReminderActionCommand): Promise<void> {
        this.commands.push(command);
        return Promise.resolve();
    }

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

    public close(actionId: ActionId): Promise<void> {
        for (let index = this.commands.length - 1; index >= 0; index -= 1) {
            if (this.commands[index]?.commandId === actionId) this.commands.splice(index, 1);
        }
        return Promise.resolve();
    }
}

export class InMemoryActionTokenPort implements ActionTokenPort {
    private readonly claimsByToken = new Map<string, ActionTokenClaims>();

    public issue(claims: ActionTokenClaims): Promise<string> {
        const token = `mock-token:${claims.actionId}`;
        this.claimsByToken.set(token, claims);
        return Promise.resolve(token);
    }

    public verify(token: string): Promise<ActionTokenClaims> {
        const claims = this.claimsByToken.get(token);
        if (claims === undefined) {
            return Promise.reject(new ImGatewayError('action_not_found', 'Action token is invalid'));
        }
        return Promise.resolve(claims);
    }

    public fingerprint(token: string): Promise<string> {
        return Promise.resolve(`hash:${token}`);
    }
}

export class MockDeviceAuthenticationPort implements DeviceAuthenticationPort {
    public constructor(private readonly deviceId: DeviceId) {}

    public authenticate(_authorization: string): Promise<DevicePrincipal> {
        return Promise.resolve({ deviceId: this.deviceId });
    }
}

async function* snapshot(commands: readonly ReminderActionCommand[]): AsyncIterable<ReminderActionCommand> {
    for (const command of commands) yield command;
}
