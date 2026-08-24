import { randomUUID } from 'node:crypto';

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
    OutboxEventId,
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
    ImDevice,
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
    DeviceRepository,
    IdentityRepository,
    ImUnitOfWork,
    ImUnitOfWorkContext,
    InboundEventRepository,
    IntentSubmissionRepository,
    OutboxRepository,
    PairingSessionRepository,
} from '../../ports/repositories.js';
import type { IsoDateTime } from '../../shared/types.js';

/** 测试专用内存工作单元，刻意不模拟回滚与锁。 */
export class InMemoryImUnitOfWork implements ImUnitOfWork, ImUnitOfWorkContext {
    public readonly devices: DeviceRepository = this;
    public readonly channelAccounts: ChannelAccountRepository = this;
    public readonly pairingSessions: PairingSessionRepository = this;
    public readonly identities: IdentityRepository = this;
    public readonly bindings: BindingRepository = this;
    public readonly inboundEvents: InboundEventRepository = this;
    public readonly intentSubmissions: IntentSubmissionRepository = this;
    public readonly deliveries: DeliveryRepository = this;
    public readonly actions: ActionRepository = this;
    public readonly outbox: OutboxRepository = this;

    private readonly deviceRows = new Map<DeviceId, ImDevice>();
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

    /** {@inheritDoc ImUnitOfWork.transaction} */
    public transaction<T>(work: (context: ImUnitOfWorkContext) => Promise<T>): Promise<T> {
        return work(this);
    }

    /** @param device 为测试 fixture 同步预置的设备。 */
    public seedDevice(device: ImDevice): void {
        this.deviceRows.set(device.deviceId, device);
    }

    /** {@inheritDoc DeviceRepository.create} */
    public create(device: ImDevice): Promise<void> {
        if (this.deviceRows.has(device.deviceId)) return Promise.reject(deviceConflict('im_devices_pkey'));
        if ([...this.deviceRows.values()].some((row) => digestsEqual(row.tokenDigest, device.tokenDigest))) {
            return Promise.reject(deviceConflict('im_devices_token_digest_key'));
        }
        this.deviceRows.set(device.deviceId, device);
        return Promise.resolve();
    }

    /** {@inheritDoc DeviceRepository.findByTokenDigest} */
    public findByTokenDigest(tokenDigest: Uint8Array): Promise<ImDevice | undefined> {
        return Promise.resolve(
            [...this.deviceRows.values()].find((device) => digestsEqual(device.tokenDigest, tokenDigest)),
        );
    }

    /** {@inheritDoc DeviceRepository.lockById} */
    public lockById(deviceId: DeviceId): Promise<ImDevice | undefined> {
        return Promise.resolve(this.deviceRows.get(deviceId));
    }

    /** {@inheritDoc DeviceRepository.list} */
    public list(userId?: UserId): Promise<readonly ImDevice[]> {
        return Promise.resolve(
            [...this.deviceRows.values()].filter((device) => userId === undefined || device.userId === userId),
        );
    }

    /** {@inheritDoc PairingSessionRepository.createPendingIfAbsent} */
    public createPendingIfAbsent(session: PairingSession): Promise<boolean> {
        const exists = [...this.pairingRows.values()].some(
            (candidate) => candidate.status === 'pending' && candidate.displayCodeHash === session.displayCodeHash,
        );
        if (exists) return Promise.resolve(false);
        this.pairingRows.set(session.id, session);
        return Promise.resolve(true);
    }

    /** {@inheritDoc PairingSessionRepository.cancelPendingByDevice} */
    public cancelPendingByDevice(deviceId: DeviceId): Promise<void> {
        for (const [id, session] of this.pairingRows) {
            if (session.deviceId === deviceId && session.status === 'pending') {
                this.pairingRows.set(id, { ...session, status: 'cancelled' });
            }
        }
        return Promise.resolve();
    }

    /** {@inheritDoc BindingRepository.acquireReplacementLock} */
    public acquireReplacementLock(): Promise<void> {
        return Promise.resolve();
    }

    /** {@inheritDoc BindingRepository.createActiveIfAbsent} */
    public createActiveIfAbsent(binding: ImBinding): Promise<ImBinding> {
        const existing = [...this.bindingRows.values()].find(
            (candidate) =>
                candidate.status === 'active' &&
                candidate.userId === binding.userId &&
                candidate.deviceId === binding.deviceId &&
                candidate.externalIdentityId === binding.externalIdentityId,
        );
        if (existing !== undefined) return Promise.resolve(existing);
        this.bindingRows.set(binding.id, binding);
        return Promise.resolve(binding);
    }

    /** {@inheritDoc BindingRepository.replaceActiveBinding} */
    public replaceActiveBinding(binding: ImBinding): Promise<ImBinding> {
        if (binding.status !== 'active' || binding.deviceId === undefined) {
            return Promise.reject(new Error('Active replacement requires a device'));
        }
        const same = [...this.bindingRows.values()].find(
            (candidate) =>
                candidate.status === 'active' &&
                candidate.userId === binding.userId &&
                candidate.deviceId === binding.deviceId &&
                candidate.externalIdentityId === binding.externalIdentityId,
        );
        if (same !== undefined) return Promise.resolve(same);
        for (const [id, candidate] of this.bindingRows) {
            if (
                candidate.status === 'active' &&
                (candidate.deviceId === binding.deviceId || candidate.externalIdentityId === binding.externalIdentityId)
            ) {
                this.bindingRows.set(id, { ...candidate, status: 'unbound', unboundAt: binding.boundAt });
            }
        }
        this.bindingRows.set(binding.id, binding);
        return Promise.resolve(binding);
    }

    /** {@inheritDoc DeviceRepository.save} */
    public save(value: ImDevice): Promise<void>;
    /** {@inheritDoc ChannelAccountRepository.save} */
    public save(value: ChannelAccount): Promise<void>;
    /** {@inheritDoc PairingSessionRepository.save} */
    public save(value: PairingSession): Promise<void>;
    /** {@inheritDoc IdentityRepository.save} */
    public save(value: ExternalIdentity): Promise<void>;
    /** {@inheritDoc BindingRepository.save} */
    public save(value: ImBinding): Promise<void>;
    /** {@inheritDoc InboundEventRepository.save} */
    public save(value: InboundEventRecord): Promise<void>;
    /** {@inheritDoc DeliveryRepository.save} */
    public save(value: Delivery): Promise<void>;
    /** {@inheritDoc ActionRepository.save} */
    public save(value: ImAction): Promise<void>;
    /**
     * 将仓储聚合的当前状态写入对应内存表。
     * @param value 要保存的聚合。
     * @returns 保存完成后兑现的 Promise。
     */
    public save(
        value:
            | ImDevice
            | ChannelAccount
            | PairingSession
            | ExternalIdentity
            | ImBinding
            | InboundEventRecord
            | Delivery
            | ImAction,
    ): Promise<void> {
        if ('tokenDigest' in value) {
            const existing = this.deviceRows.get(value.deviceId);
            this.deviceRows.set(value.deviceId, existing === undefined ? value : { ...value, userId: existing.userId });
        } else if ('koishiBotId' in value) this.channelRows.set(value.id, value);
        else if ('displayCodeHash' in value) this.pairingRows.set(value.id, value);
        else if ('externalUserIdCiphertext' in value) {
            this.identityRows.set(value.id, value);
        } else if ('externalIdentityId' in value) this.bindingRows.set(value.id, value);
        else if ('externalEventId' in value && 'eventType' in value) {
            this.inboundRows.set(inboundKey(value.channelAccountId, value.externalEventId), value);
        } else if ('businessEventId' in value) this.deliveryRows.set(value.id, value);
        else this.actionRows.set(value.id, value);
        return Promise.resolve();
    }

    /** {@inheritDoc ActionRepository.createIfAbsent} */
    public createIfAbsent(action: ImAction): Promise<{ readonly action: ImAction; readonly created: boolean }>;
    /** {@inheritDoc DeliveryRepository.createIfAbsent} */
    public createIfAbsent(delivery: Delivery): Promise<{ id: DeliveryId; created: boolean }>;
    /** {@inheritDoc IntentSubmissionRepository.createIfAbsent} */
    public createIfAbsent(
        record: IntentSubmissionRecord,
    ): Promise<{ created: boolean; record: IntentSubmissionRecord }>;
    /** {@inheritDoc IdentityRepository.createIfAbsent} */
    public createIfAbsent(identity: ExternalIdentity): Promise<ExternalIdentity>;
    /**
     * 按业务键幂等创建投递或受理记录；同键已存在时保留首条。
     * @param value 待创建的投递或受理记录。
     * @returns 投递：权威标识与是否新建；受理记录：是否新建与当前权威记录。
     */
    public createIfAbsent(
        value: Delivery | IntentSubmissionRecord | ImAction | ExternalIdentity,
    ): Promise<
        | { readonly action: ImAction; readonly created: boolean }
        | { id: DeliveryId; created: boolean }
        | { created: boolean; record: IntentSubmissionRecord }
        | ExternalIdentity
    > {
        if ('externalUserIdCiphertext' in value) {
            const existing = [...this.identityRows.values()].find(
                (candidate) =>
                    candidate.channelAccountId === value.channelAccountId &&
                    candidate.externalUserIdHash === value.externalUserIdHash,
            );
            if (existing !== undefined) return Promise.resolve(existing);
            this.identityRows.set(value.id, value);
            return Promise.resolve(value);
        }
        if ('actionKeyHash' in value) {
            const existing =
                this.actionRows.get(value.id) ??
                [...this.actionRows.values()].find((candidate) => candidate.actionKeyHash === value.actionKeyHash);
            if (existing !== undefined) return Promise.resolve({ action: existing, created: false });
            this.actionRows.set(value.id, value);
            return Promise.resolve({ action: value, created: true });
        }
        if ('requestFingerprint' in value) {
            const existing = this.intentSubmissionRows.get(intentSubmissionKey(value.businessEventId, value.kind));
            if (existing !== undefined) {
                return Promise.resolve({ created: false, record: existing });
            }
            this.intentSubmissionRows.set(intentSubmissionKey(value.businessEventId, value.kind), value);
            return Promise.resolve({ created: true, record: value });
        }
        const existing = [...this.deliveryRows.values()].find(
            (candidate) =>
                candidate.businessEventId === value.businessEventId &&
                candidate.bindingId === value.bindingId &&
                candidate.kind === value.kind,
        );
        if (existing !== undefined) return Promise.resolve({ id: existing.id, created: false });
        this.deliveryRows.set(value.id, value);
        return Promise.resolve({ id: value.id, created: true });
    }

    /** {@inheritDoc IntentSubmissionRepository.finalizeClaim} */
    public finalizeClaim(record: IntentSubmissionRecord): Promise<void> {
        this.intentSubmissionRows.set(intentSubmissionKey(record.businessEventId, record.kind), record);
        return Promise.resolve();
    }

    /** {@inheritDoc DeliveryRepository.claimForDispatch} */
    public claimForDispatch(
        deliveryId: DeliveryId,
        now: IsoDateTime,
        leaseSeconds: number,
    ): Promise<Delivery | undefined> {
        const delivery = this.deliveryRows.get(deliveryId);
        if (delivery === undefined) return Promise.resolve(undefined);
        const claim = randomUUID();
        const isClaimable =
            delivery.status === 'pending' ||
            delivery.status === 'retryable_failed' ||
            (delivery.status === 'sending' &&
                (delivery.claimedAt === undefined ||
                    Date.parse(now) - Date.parse(delivery.claimedAt) >= leaseSeconds * 1000));
        if (!isClaimable) return Promise.resolve(undefined);
        const claimed: Delivery = {
            ...delivery,
            status: 'sending',
            claimedAt: now,
            claimToken: claim,
            updatedAt: now,
        };
        this.deliveryRows.set(deliveryId, claimed);
        return Promise.resolve(claimed);
    }

    /** {@inheritDoc DeliveryRepository.saveIfClaimed} */
    public saveIfClaimed(delivery: Delivery, claimToken: string): Promise<Delivery | undefined> {
        const existing = this.deliveryRows.get(delivery.id);
        if (existing === undefined || existing.status !== 'sending' || existing.claimToken !== claimToken) {
            return Promise.resolve(undefined);
        }
        this.deliveryRows.set(delivery.id, delivery);
        return Promise.resolve(delivery);
    }

    /** {@inheritDoc DeviceRepository.findById} */
    public findById(id: DeviceId): Promise<ImDevice | undefined>;
    /** {@inheritDoc ChannelAccountRepository.findById} */
    public findById(id: ChannelAccountId): Promise<ChannelAccount | undefined>;
    /** {@inheritDoc PairingSessionRepository.findById} */
    public findById(id: PairingSessionId): Promise<PairingSession | undefined>;
    /** {@inheritDoc IdentityRepository.findById} */
    public findById(id: ExternalIdentityId): Promise<ExternalIdentity | undefined>;
    /** {@inheritDoc BindingRepository.findById} */
    public findById(id: BindingId): Promise<ImBinding | undefined>;
    /** {@inheritDoc InboundEventRepository.findById} */
    public findById(id: InboundEventId): Promise<InboundEventRecord | undefined>;
    /** {@inheritDoc DeliveryRepository.findById} */
    public findById(id: DeliveryId): Promise<Delivery | undefined>;
    /** {@inheritDoc ActionRepository.findById} */
    public findById(id: ActionId): Promise<ImAction | undefined>;
    /**
     * 从各内存表中按品牌标识查询聚合。
     * @param id 聚合标识。
     * @returns 匹配的聚合，不存在时返回 undefined。
     */
    public findById(
        id:
            | DeviceId
            | ChannelAccountId
            | PairingSessionId
            | ExternalIdentityId
            | BindingId
            | InboundEventId
            | DeliveryId
            | ActionId,
    ): Promise<
        | ImDevice
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
            this.deviceRows.get(id as DeviceId) ??
                this.channelRows.get(id as ChannelAccountId) ??
                this.pairingRows.get(id as PairingSessionId) ??
                this.identityRows.get(id as ExternalIdentityId) ??
                this.bindingRows.get(id as BindingId) ??
                [...this.inboundRows.values()].find((event) => event.id === (id as InboundEventId)) ??
                this.deliveryRows.get(id as DeliveryId) ??
                this.actionRows.get(id as ActionId),
        );
    }

    /** {@inheritDoc PairingSessionRepository.findPendingByDisplayCodeHash} */
    public findPendingByDisplayCodeHash(hash: string): Promise<PairingSession | undefined> {
        return Promise.resolve(
            [...this.pairingRows.values()].find(
                (session) => session.displayCodeHash === hash && session.status === 'pending',
            ),
        );
    }

    /** {@inheritDoc PairingSessionRepository.lockPendingByDisplayCodeHash} */
    public lockPendingByDisplayCodeHash(hash: string): Promise<PairingSession | undefined> {
        return this.findPendingByDisplayCodeHash(hash);
    }

    /** {@inheritDoc PairingSessionRepository.lockPendingByIdAndDisplayCodeHash} */
    public lockPendingByIdAndDisplayCodeHash(id: PairingSessionId, hash: string): Promise<PairingSession | undefined> {
        const session = this.pairingRows.get(id);
        return Promise.resolve(session?.status === 'pending' && session.displayCodeHash === hash ? session : undefined);
    }

    /** {@inheritDoc PairingSessionRepository.transitionPending} */
    public transitionPending(id: PairingSessionId, status: 'cancelled' | 'expired'): Promise<boolean> {
        const session = this.pairingRows.get(id);
        if (session === undefined || session.status !== 'pending') return Promise.resolve(false);
        this.pairingRows.set(id, { ...session, status });
        return Promise.resolve(true);
    }

    /** {@inheritDoc PairingSessionRepository.findExpiredPairingSessions} */
    public findExpiredPairingSessions(now: IsoDateTime): Promise<readonly PairingSession[]> {
        return Promise.resolve(
            [...this.pairingRows.values()].filter(
                (session) => session.status === 'pending' && session.expiresAt <= now,
            ),
        );
    }

    /** {@inheritDoc IdentityRepository.findByChannelAndHash} */
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

    /** {@inheritDoc BindingRepository.listActiveByUser} */
    public listActiveByUser(userId: UserId): Promise<readonly ImBinding[]> {
        return Promise.resolve(
            [...this.bindingRows.values()]
                .filter((binding) => binding.userId === userId && binding.status === 'active')
                .sort((left, right) => left.priority - right.priority),
        );
    }

    /** {@inheritDoc BindingRepository.findActiveByDevice} */
    public findActiveByDevice(deviceId: DeviceId): Promise<readonly ImBinding[]> {
        return Promise.resolve(
            [...this.bindingRows.values()].filter(
                (binding) => binding.deviceId === deviceId && binding.status === 'active',
            ),
        );
    }

    /** {@inheritDoc BindingRepository.findActiveByIdentity} */
    public findActiveByIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined> {
        return Promise.resolve(
            [...this.bindingRows.values()].find(
                (binding) => binding.externalIdentityId === externalIdentityId && binding.status === 'active',
            ),
        );
    }

    /** {@inheritDoc InboundEventRepository.findByExternalEvent} */
    public findByExternalEvent(
        channelAccountId: ChannelAccountId,
        externalEventId: string,
    ): Promise<InboundEventRecord | undefined> {
        return Promise.resolve(this.inboundRows.get(inboundKey(channelAccountId, externalEventId)));
    }

    /** {@inheritDoc DeliveryRepository.findByBusinessKey} */
    public findByBusinessKey(
        businessEventId: EventId,
        bindingId: BindingId,
        kind: Delivery['kind'],
    ): Promise<Delivery | undefined>;
    /** {@inheritDoc IntentSubmissionRepository.findByBusinessKey} */
    public findByBusinessKey(
        businessEventId: EventId,
        kind: IntentSubmissionRecord['kind'],
    ): Promise<IntentSubmissionRecord | undefined>;
    /**
     * 按投递键或受理键查询对应记录。
     * @param businessEventId 业务事件标识。
     * @param bindingIdOrKind 绑定标识或投递类型。
     * @param kind 使用投递键查询时的投递类型。
     * @returns 匹配的投递或受理记录。
     */
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

    /** {@inheritDoc DeliveryRepository.findByExternalMessage} */
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

    /** {@inheritDoc DeliveryRepository.findActiveActionWindow} */
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

    /** {@inheritDoc DeliveryRepository.findAttempt} */
    public findAttempt(deliveryId: DeliveryId, attemptNo: number): Promise<DeliveryAttempt | undefined> {
        return Promise.resolve(this.attemptRows.get(attemptKey(deliveryId, attemptNo)));
    }

    /** {@inheritDoc DeliveryRepository.saveAttempt} */
    public saveAttempt(attempt: DeliveryAttempt): Promise<void> {
        this.attemptRows.set(attemptKey(attempt.deliveryId, attempt.attemptNo), attempt);
        return Promise.resolve();
    }

    /** {@inheritDoc DeliveryRepository.nextAttemptNo} */
    public nextAttemptNo(deliveryId: DeliveryId): Promise<number> {
        const attempts = [...this.attemptRows.values()].filter((attempt) => attempt.deliveryId === deliveryId);
        return Promise.resolve(attempts.length + 1);
    }

    /** {@inheritDoc DeliveryRepository.listAttempts} */
    public listAttempts(deliveryId: DeliveryId): Promise<readonly DeliveryAttempt[]> {
        return Promise.resolve(
            [...this.attemptRows.values()]
                .filter((attempt) => attempt.deliveryId === deliveryId)
                .sort((left, right) => left.attemptNo - right.attemptNo),
        );
    }

    /** {@inheritDoc DeliveryRepository.findReceiptByDedupeKey} */
    public findReceiptByDedupeKey(dedupeKey: string): Promise<DeliveryReceipt | undefined> {
        return Promise.resolve(this.receiptRows.get(dedupeKey));
    }

    /** {@inheritDoc DeliveryRepository.listReceipts} */
    public listReceipts(deliveryId: DeliveryId): Promise<readonly DeliveryReceipt[]> {
        return Promise.resolve([...this.receiptRows.values()].filter((receipt) => receipt.deliveryId === deliveryId));
    }

    /** {@inheritDoc DeliveryRepository.saveReceipt} */
    public saveReceipt(receipt: DeliveryReceipt): Promise<void> {
        if (!this.receiptRows.has(receipt.dedupeKey)) {
            this.receiptRows.set(receipt.dedupeKey, receipt);
        }
        return Promise.resolve();
    }

    /** {@inheritDoc ActionRepository.findByOperationId} */
    public findByOperationId(operationId: OperationId): Promise<ImAction | undefined> {
        return Promise.resolve([...this.actionRows.values()].find((action) => action.operationId === operationId));
    }

    /** {@inheritDoc ActionRepository.findByResultOperationId} */
    public findByResultOperationId(operationId: OperationId): Promise<ImAction | undefined> {
        return Promise.resolve(
            [...this.actionRows.values()].find((action) => action.result?.operationId === operationId),
        );
    }

    /** {@inheritDoc ActionRepository.findByActionKeyHash} */
    public findByActionKeyHash(actionKeyHash: string): Promise<ImAction | undefined> {
        return Promise.resolve([...this.actionRows.values()].find((action) => action.actionKeyHash === actionKeyHash));
    }

    /** {@inheritDoc ActionRepository.findPendingByDeviceAndTrigger} */
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

    /** {@inheritDoc ActionRepository.findExpiredActions} */
    public findExpiredActions(now: IsoDateTime): Promise<readonly ImAction[]> {
        return Promise.resolve(
            [...this.actionRows.values()].filter(
                (action) =>
                    action.expiresAt <= now &&
                    (action.status === 'pending' || action.status === 'dispatched' || action.status === 'processing'),
            ),
        );
    }

    /** {@inheritDoc OutboxRepository.append} */
    public append(event: ImOutboxEvent): Promise<void> {
        this.outboxRows.push(event);
        return Promise.resolve();
    }

    /** {@inheritDoc OutboxRepository.claimPending} */
    public claimPending(
        eventTypes: readonly string[],
        now: IsoDateTime,
        leaseUntil: IsoDateTime,
        limit: number,
    ): Promise<readonly ImOutboxEvent[]> {
        const claimed = this.outboxRows
            .filter(
                (event) =>
                    event.status === 'pending' && event.availableAt <= now && eventTypes.includes(event.eventType),
            )
            .sort(
                (left, right) =>
                    left.availableAt.localeCompare(right.availableAt) || left.createdAt.localeCompare(right.createdAt),
            )
            .slice(0, limit)
            .map((event) => ({ ...event, attempts: event.attempts + 1, availableAt: leaseUntil }));
        for (const event of claimed) this.replaceOutbox(event);
        return Promise.resolve(claimed);
    }

    /** {@inheritDoc OutboxRepository.markPublished} */
    public markPublished(eventId: OutboxEventId, publishedAt: IsoDateTime): Promise<void> {
        const event = this.outboxRows.find((candidate) => candidate.id === eventId);
        if (event !== undefined) this.replaceOutbox({ ...event, status: 'published', publishedAt });
        return Promise.resolve();
    }

    /** {@inheritDoc OutboxRepository.markFailed} */
    public markFailed(eventId: OutboxEventId): Promise<void> {
        const event = this.outboxRows.find((candidate) => candidate.id === eventId);
        if (event !== undefined) this.replaceOutbox({ ...event, status: 'failed' });
        return Promise.resolve();
    }

    private replaceOutbox(event: ImOutboxEvent): void {
        const index = this.outboxRows.findIndex((candidate) => candidate.id === event.id);
        if (index >= 0) this.outboxRows[index] = event;
    }
}

function deviceConflict(constraint: string): Error & { constraint: string } {
    return Object.assign(new Error('Device already exists'), { constraint });
}

function digestsEqual(left: Uint8Array, right: Uint8Array): boolean {
    return left.length === right.length && left.every((value, index) => value === right[index]);
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
