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
} from '../contracts/ids.js';
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
} from '../domain/models.js';
import type { IsoDateTime } from '../shared/types.js';

export interface ChannelAccountRepository {
    findById(id: ChannelAccountId): Promise<ChannelAccount | undefined>;
    save(account: ChannelAccount): Promise<void>;
}

export interface PairingSessionRepository {
    findById(id: PairingSessionId): Promise<PairingSession | undefined>;
    findPendingByDisplayCodeHash(hash: string): Promise<PairingSession | undefined>;
    findExpiredPairingSessions(now: IsoDateTime): Promise<readonly PairingSession[]>;
    save(session: PairingSession): Promise<void>;
}

export interface IdentityRepository {
    findById(id: ExternalIdentityId): Promise<ExternalIdentity | undefined>;
    findByChannelAndHash(
        channelAccountId: ChannelAccountId,
        externalUserIdHash: string,
    ): Promise<ExternalIdentity | undefined>;
    save(identity: ExternalIdentity): Promise<void>;
}

export interface BindingRepository {
    findById(id: BindingId): Promise<ImBinding | undefined>;
    listActiveByUser(userId: UserId): Promise<readonly ImBinding[]>;
    findActiveByDevice(deviceId: DeviceId): Promise<readonly ImBinding[]>;
    findActiveByIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined>;
    save(binding: ImBinding): Promise<void>;
}

export interface InboundEventRepository {
    findById(id: InboundEventId): Promise<InboundEventRecord | undefined>;
    findByExternalEvent(
        channelAccountId: ChannelAccountId,
        externalEventId: string,
    ): Promise<InboundEventRecord | undefined>;
    save(event: InboundEventRecord): Promise<void>;
}

export interface DeliveryRepository {
    findById(id: DeliveryId): Promise<Delivery | undefined>;
    findByExternalMessage(channelAccountId: ChannelAccountId, externalMessageId: string): Promise<Delivery | undefined>;
    findActiveActionWindow(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<Delivery | undefined>;
    findByBusinessKey(
        businessEventId: EventId,
        bindingId: BindingId,
        kind: Delivery['kind'],
    ): Promise<Delivery | undefined>;
    save(delivery: Delivery): Promise<void>;
    findAttempt(deliveryId: DeliveryId, attemptNo: number): Promise<DeliveryAttempt | undefined>;
    nextAttemptNo(deliveryId: DeliveryId): Promise<number>;
    listAttempts(deliveryId: DeliveryId): Promise<readonly DeliveryAttempt[]>;
    saveAttempt(attempt: DeliveryAttempt): Promise<void>;
    findReceiptByDedupeKey(dedupeKey: string): Promise<DeliveryReceipt | undefined>;
    listReceipts(deliveryId: DeliveryId): Promise<readonly DeliveryReceipt[]>;
    saveReceipt(receipt: DeliveryReceipt): Promise<void>;
}

export interface IntentSubmissionRepository {
    findByBusinessKey(
        businessEventId: EventId,
        kind: IntentSubmissionRecord['kind'],
    ): Promise<IntentSubmissionRecord | undefined>;
    save(record: IntentSubmissionRecord): Promise<void>;
}

export interface ActionRepository {
    findById(id: ActionId): Promise<ImAction | undefined>;
    findByOperationId(operationId: OperationId): Promise<ImAction | undefined>;
    findByActionKeyHash(actionKeyHash: string): Promise<ImAction | undefined>;
    findPendingByDeviceAndTrigger(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<readonly ImAction[]>;
    findExpiredActions(now: IsoDateTime): Promise<readonly ImAction[]>;
    save(action: ImAction): Promise<void>;
}

export interface OutboxRepository {
    append(event: ImOutboxEvent): Promise<void>;
}

export interface ImUnitOfWorkContext {
    readonly channelAccounts: ChannelAccountRepository;
    readonly pairingSessions: PairingSessionRepository;
    readonly identities: IdentityRepository;
    readonly bindings: BindingRepository;
    readonly inboundEvents: InboundEventRepository;
    readonly intentSubmissions: IntentSubmissionRepository;
    readonly deliveries: DeliveryRepository;
    readonly actions: ActionRepository;
    readonly outbox: OutboxRepository;
}

export interface ImUnitOfWork {
    transaction<T>(work: (context: ImUnitOfWorkContext) => Promise<T>): Promise<T>;
}
