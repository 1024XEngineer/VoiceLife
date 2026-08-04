import type {
    ActionId,
    BindingId,
    ChannelAccountId,
    DeliveryId,
    DeviceId,
    EventId,
    ExternalIdentityId,
    InboundEventId,
    OperationId,
    PairingSessionId,
    ReminderTriggerId,
    UserId,
} from '../../contracts/ids.js';
import type {
    ChannelAccount,
    Delivery,
    DeliveryAttempt,
    DeliveryReceipt,
    ExternalIdentity,
    ImAction,
    ImBinding,
    ImOutboxEvent,
    InboundEventRecord,
    IntentSubmissionRecord,
    PairingSession,
} from '../../domain/models.js';
import type {
    ActionRepository,
    BindingRepository,
    ChannelAccountRepository,
    DeliveryRepository,
    IdentityRepository,
    ImUnitOfWork,
    ImUnitOfWorkContext,
    InboundEventRepository,
    IntentSubmissionRepository,
    OutboxRepository,
    PairingSessionRepository,
} from '../../ports/repositories.js';
import type { IsoDateTime } from '../../shared/types.js';

/** Test-only adapter. It deliberately does not emulate rollback or locking. */
export class InMemoryImUnitOfWork implements ImUnitOfWork, ImUnitOfWorkContext {
    public readonly channelAccounts: ChannelAccountRepository = this;
    public readonly pairingSessions: PairingSessionRepository = this;
    public readonly identities: IdentityRepository = this;
    public readonly bindings: BindingRepository = this;
    public readonly inboundEvents: InboundEventRepository = this;
    public readonly intentSubmissions: IntentSubmissionRepository = this;
    public readonly deliveries: DeliveryRepository = this;
    public readonly actions: ActionRepository = this;
    public readonly outbox: OutboxRepository = this;

    private readonly channelRows = new Map<ChannelAccountId, ChannelAccount>();
    private readonly pairingRows = new Map<PairingSessionId, PairingSession>();
    private readonly identityRows = new Map<ExternalIdentityId, ExternalIdentity>();
    private readonly bindingRows = new Map<BindingId, ImBinding>();
    private readonly inboundRows = new Map<string, InboundEventRecord>();
    private readonly intentSubmissionRows = new Map<string, IntentSubmissionRecord>();
    private readonly deliveryRows = new Map<DeliveryId, Delivery>();
    private readonly attemptRows = new Map<string, DeliveryAttempt>();
    private readonly receiptRows = new Map<string, DeliveryReceipt>();
    private readonly actionRows = new Map<ActionId, ImAction>();
    private readonly outboxRows: ImOutboxEvent[] = [];

    public transaction<T>(work: (context: ImUnitOfWorkContext) => Promise<T>): Promise<T> {
        return work(this);
    }

    public save(value: ChannelAccount): Promise<void>;
    public save(value: PairingSession): Promise<void>;
    public save(value: ExternalIdentity): Promise<void>;
    public save(value: ImBinding): Promise<void>;
    public save(value: InboundEventRecord): Promise<void>;
    public save(value: IntentSubmissionRecord): Promise<void>;
    public save(value: Delivery): Promise<void>;
    public save(value: ImAction): Promise<void>;
    public save(
        value:
            | ChannelAccount
            | PairingSession
            | ExternalIdentity
            | ImBinding
            | InboundEventRecord
            | IntentSubmissionRecord
            | Delivery
            | ImAction,
    ): Promise<void> {
        if ('koishiBotId' in value) this.channelRows.set(value.id, value);
        else if ('displayCodeHash' in value) this.pairingRows.set(value.id, value);
        else if ('externalUserIdCiphertext' in value) {
            this.identityRows.set(value.id, value);
        } else if ('externalIdentityId' in value) this.bindingRows.set(value.id, value);
        else if ('externalEventId' in value && 'eventType' in value) {
            this.inboundRows.set(inboundKey(value.channelAccountId, value.externalEventId), value);
        } else if ('requestFingerprint' in value) {
            this.intentSubmissionRows.set(intentSubmissionKey(value.businessEventId, value.kind), value);
        } else if ('businessEventId' in value) this.deliveryRows.set(value.id, value);
        else this.actionRows.set(value.id, value);
        return Promise.resolve();
    }

    public findById(id: ChannelAccountId): Promise<ChannelAccount | undefined>;
    public findById(id: PairingSessionId): Promise<PairingSession | undefined>;
    public findById(id: ExternalIdentityId): Promise<ExternalIdentity | undefined>;
    public findById(id: BindingId): Promise<ImBinding | undefined>;
    public findById(id: InboundEventId): Promise<InboundEventRecord | undefined>;
    public findById(id: DeliveryId): Promise<Delivery | undefined>;
    public findById(id: ActionId): Promise<ImAction | undefined>;
    public findById(
        id:
            | ChannelAccountId
            | PairingSessionId
            | ExternalIdentityId
            | BindingId
            | InboundEventId
            | DeliveryId
            | ActionId,
    ): Promise<
        | ChannelAccount
        | PairingSession
        | ExternalIdentity
        | ImBinding
        | InboundEventRecord
        | Delivery
        | ImAction
        | undefined
    > {
        return Promise.resolve(
            this.channelRows.get(id as ChannelAccountId) ??
                this.pairingRows.get(id as PairingSessionId) ??
                this.identityRows.get(id as ExternalIdentityId) ??
                this.bindingRows.get(id as BindingId) ??
                [...this.inboundRows.values()].find((event) => event.id === (id as InboundEventId)) ??
                this.deliveryRows.get(id as DeliveryId) ??
                this.actionRows.get(id as ActionId),
        );
    }

    public findPendingByDisplayCodeHash(hash: string): Promise<PairingSession | undefined> {
        return Promise.resolve(
            [...this.pairingRows.values()].find(
                (session) => session.displayCodeHash === hash && session.status === 'pending',
            ),
        );
    }

    public findExpiredPairingSessions(now: IsoDateTime): Promise<readonly PairingSession[]> {
        return Promise.resolve(
            [...this.pairingRows.values()].filter(
                (session) => session.status === 'pending' && session.expiresAt <= now,
            ),
        );
    }

    public findByChannelAndHash(
        channelAccountId: ChannelAccountId,
        externalUserIdHash: string,
    ): Promise<ExternalIdentity | undefined> {
        return Promise.resolve(
            [...this.identityRows.values()].find(
                (identity) =>
                    identity.channelAccountId === channelAccountId &&
                    identity.externalUserIdHash === externalUserIdHash,
            ),
        );
    }

    public listActiveByUser(userId: UserId): Promise<readonly ImBinding[]> {
        return Promise.resolve(
            [...this.bindingRows.values()]
                .filter((binding) => binding.userId === userId && binding.status === 'active')
                .sort((left, right) => left.priority - right.priority),
        );
    }

    public findActiveByDevice(deviceId: DeviceId): Promise<readonly ImBinding[]> {
        return Promise.resolve(
            [...this.bindingRows.values()].filter(
                (binding) => binding.deviceId === deviceId && binding.status === 'active',
            ),
        );
    }

    public findActiveByIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined> {
        return Promise.resolve(
            [...this.bindingRows.values()].find(
                (binding) => binding.externalIdentityId === externalIdentityId && binding.status === 'active',
            ),
        );
    }

    public findByExternalEvent(
        channelAccountId: ChannelAccountId,
        externalEventId: string,
    ): Promise<InboundEventRecord | undefined> {
        return Promise.resolve(this.inboundRows.get(inboundKey(channelAccountId, externalEventId)));
    }

    public findByBusinessKey(
        businessEventId: EventId,
        bindingId: BindingId,
        kind: Delivery['kind'],
    ): Promise<Delivery | undefined>;
    public findByBusinessKey(
        businessEventId: EventId,
        kind: IntentSubmissionRecord['kind'],
    ): Promise<IntentSubmissionRecord | undefined>;
    public findByBusinessKey(
        businessEventId: EventId,
        bindingIdOrKind: BindingId | IntentSubmissionRecord['kind'],
        kind?: Delivery['kind'],
    ): Promise<Delivery | IntentSubmissionRecord | undefined> {
        if (kind === undefined) {
            return Promise.resolve(
                this.intentSubmissionRows.get(
                    intentSubmissionKey(businessEventId, bindingIdOrKind as IntentSubmissionRecord['kind']),
                ),
            );
        }
        return Promise.resolve(
            [...this.deliveryRows.values()].find(
                (delivery) =>
                    delivery.businessEventId === businessEventId &&
                    delivery.bindingId === bindingIdOrKind &&
                    delivery.kind === kind,
            ),
        );
    }

    public findByExternalMessage(
        channelAccountId: ChannelAccountId,
        externalMessageId: string,
    ): Promise<Delivery | undefined> {
        const direct = [...this.deliveryRows.values()].find(
            (delivery) =>
                delivery.channelAccountId === channelAccountId && delivery.externalMessageId === externalMessageId,
        );
        if (direct !== undefined) return Promise.resolve(direct);
        const attempt = [...this.attemptRows.values()].find(
            (candidate) => candidate.platformMessageId === externalMessageId,
        );
        const delivery = attempt === undefined ? undefined : this.deliveryRows.get(attempt.deliveryId);
        return Promise.resolve(delivery?.channelAccountId === channelAccountId ? delivery : undefined);
    }

    public findActiveActionWindow(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<Delivery | undefined> {
        return Promise.resolve(
            [...this.deliveryRows.values()].find((delivery) => {
                const payload = delivery.semanticPayload;
                if (
                    delivery.expiresAt === undefined ||
                    delivery.expiresAt <= now ||
                    typeof payload !== 'object' ||
                    payload === null ||
                    Array.isArray(payload) ||
                    payload.reminderType !== 'strong' ||
                    payload.reminderTriggerId !== reminderTriggerId ||
                    typeof payload.recipient !== 'object' ||
                    payload.recipient === null ||
                    Array.isArray(payload.recipient)
                ) {
                    return false;
                }
                return payload.recipient.deviceId === deviceId;
            }),
        );
    }

    public findAttempt(deliveryId: DeliveryId, attemptNo: number): Promise<DeliveryAttempt | undefined> {
        return Promise.resolve(this.attemptRows.get(attemptKey(deliveryId, attemptNo)));
    }

    public saveAttempt(attempt: DeliveryAttempt): Promise<void> {
        this.attemptRows.set(attemptKey(attempt.deliveryId, attempt.attemptNo), attempt);
        return Promise.resolve();
    }

    public nextAttemptNo(deliveryId: DeliveryId): Promise<number> {
        const attempts = [...this.attemptRows.values()].filter((attempt) => attempt.deliveryId === deliveryId);
        return Promise.resolve(attempts.length + 1);
    }

    public listAttempts(deliveryId: DeliveryId): Promise<readonly DeliveryAttempt[]> {
        return Promise.resolve(
            [...this.attemptRows.values()]
                .filter((attempt) => attempt.deliveryId === deliveryId)
                .sort((left, right) => left.attemptNo - right.attemptNo),
        );
    }

    public findReceiptByDedupeKey(dedupeKey: string): Promise<DeliveryReceipt | undefined> {
        return Promise.resolve(this.receiptRows.get(dedupeKey));
    }

    public listReceipts(deliveryId: DeliveryId): Promise<readonly DeliveryReceipt[]> {
        return Promise.resolve([...this.receiptRows.values()].filter((receipt) => receipt.deliveryId === deliveryId));
    }

    public saveReceipt(receipt: DeliveryReceipt): Promise<void> {
        this.receiptRows.set(receipt.dedupeKey, receipt);
        return Promise.resolve();
    }

    public findByOperationId(operationId: OperationId): Promise<ImAction | undefined> {
        return Promise.resolve([...this.actionRows.values()].find((action) => action.operationId === operationId));
    }

    public findByActionKeyHash(actionKeyHash: string): Promise<ImAction | undefined> {
        return Promise.resolve([...this.actionRows.values()].find((action) => action.actionKeyHash === actionKeyHash));
    }

    public findPendingByDeviceAndTrigger(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<readonly ImAction[]> {
        return Promise.resolve(
            [...this.actionRows.values()].filter(
                (action) =>
                    action.deviceId === deviceId &&
                    action.reminderTriggerId === reminderTriggerId &&
                    action.expiresAt > now &&
                    (action.status === 'pending' || action.status === 'dispatched' || action.status === 'processing'),
            ),
        );
    }

    public findExpiredActions(now: IsoDateTime): Promise<readonly ImAction[]> {
        return Promise.resolve(
            [...this.actionRows.values()].filter(
                (action) =>
                    action.expiresAt <= now &&
                    (action.status === 'pending' || action.status === 'dispatched' || action.status === 'processing'),
            ),
        );
    }

    public append(event: ImOutboxEvent): Promise<void> {
        this.outboxRows.push(event);
        return Promise.resolve();
    }
}

function inboundKey(channelAccountId: ChannelAccountId, externalEventId: string): string {
    return `${channelAccountId}:${externalEventId}`;
}

function intentSubmissionKey(businessEventId: EventId, kind: IntentSubmissionRecord['kind']): string {
    return `${businessEventId}:${kind}`;
}

function attemptKey(deliveryId: DeliveryId, attemptNo: number): string {
    return `${deliveryId}:${attemptNo}`;
}
