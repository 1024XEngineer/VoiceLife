import type {
    ActionId,
    BindingId,
    ChannelAccountId,
    DeliveryId,
    DeviceId,
    ExternalIdentityId,
    OperationId,
    PairingSessionId,
    ReminderTriggerId,
    UserId,
} from '../contracts/ids.js';
import {
    DEVICE_CONTRACT_VERSION,
    MAX_PAIRING_SESSION_MINUTES,
    MIN_PAIRING_SESSION_MINUTES,
    type NotificationIntent,
    type NotificationActionOption,
    type NotificationSubmission,
    type ReminderActionCommand,
    type ReminderActionResult,
    type ScheduleReceiptIntent,
} from '../contracts/device-gateway.js';
import type { NormalizedDeliveryReceipt, NormalizedImEvent } from '../contracts/platform-events.js';
import type {
    ActionStatus,
    ChannelAccount,
    ConversationRef,
    Delivery,
    DeliveryStatus,
    ImAction,
    ImBinding,
    PairingSession,
    PresentationType,
} from '../domain/models.js';
import type {
    ActionCommandStreamPort,
    ActionTokenClaims,
    ActionTokenPort,
    ChannelCapabilityResolver,
    ChannelHealth,
    ChannelHealthPort,
    Clock,
    ConversationResolverPort,
    DeliveryRendererPort,
    ExternalIdentityProtector,
    IdGenerator,
    ImChannelPort,
    ImSendAcceptance,
    PairingCodePort,
} from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';
import { ImGatewayError } from '../shared/errors.js';
import { canonicalizeJson } from '../shared/json.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';
import type {
    ActionApplication,
    ActionUiApplication,
    ActionUiView,
    BindingApplication,
    ChannelAccountApplication,
    ConfirmPairingCommand,
    CreatedPairingSession,
    CreatePairingSessionCommand,
    DeliveryApplication,
    DeliveryDetails,
    DeliveryDispatchApplication,
    InboundEventApplication,
    NotificationApplication,
    PairingApplication,
    PlatformEventApplication,
    ReceiptApplication,
    RegisterChannelAccountCommand,
    TriggerPreparedActionCommand,
} from './api.js';

const DEFAULT_ACTION_WINDOW_MINUTES = 10;
const DEFAULT_PAIRING_WINDOW_MINUTES = 10;

/** 派发领取租约时长；sending claim 超过该时长未续期即视为崩溃，允许其他 worker 重领。 */
const DISPATCH_CLAIM_LEASE_SECONDS = 60;

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

/** 配对会话签发、确认、取消与过期处理的默认实现。 */
export class DefaultPairingApplication implements PairingApplication {
    /**
     * 创建配对应用服务。
     * @param unitOfWork 事务工作单元。
     * @param ids 标识生成器。
     * @param clock 业务时钟。
     * @param pairingCodes 配对码端口。
     * @param identityProtector 外部身份保护端口。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
        private readonly pairingCodes: PairingCodePort,
        private readonly identityProtector: ExternalIdentityProtector,
    ) {}

    /** {@inheritDoc PairingApplication.create} */
    public async create(command: CreatePairingSessionCommand): Promise<CreatedPairingSession> {
        if (
            command.expiresInMinutes !== undefined &&
            (!Number.isInteger(command.expiresInMinutes) ||
                command.expiresInMinutes < MIN_PAIRING_SESSION_MINUTES ||
                command.expiresInMinutes > MAX_PAIRING_SESSION_MINUTES)
        ) {
            throw new ImGatewayError(
                'invalid_contract',
                `Pairing expiry must be an integer from ${MIN_PAIRING_SESSION_MINUTES} to ${MAX_PAIRING_SESSION_MINUTES} minutes`,
            );
        }
        const code = await this.pairingCodes.issue();
        const now = this.clock.now();
        const session: PairingSession = {
            id: this.ids.nextPairingSessionId(),
            displayCodeHash: code.hash,
            ...(command.userId === undefined ? {} : { userId: command.userId }),
            deviceId: command.deviceId,
            ...(command.allowedPlatforms === undefined ? {} : { allowedPlatforms: command.allowedPlatforms }),
            status: 'pending',
            expiresAt: this.clock.addMinutes(now, command.expiresInMinutes ?? DEFAULT_PAIRING_WINDOW_MINUTES),
            createdAt: now,
        };
        await this.unitOfWork.transaction((tx) => tx.pairingSessions.save(session));
        return { session, displayCode: code.displayCode };
    }

    /** {@inheritDoc PairingApplication.find} */
    public async find(pairingSessionId: PairingSessionId): Promise<PairingSession | undefined> {
        await this.expireDue();
        return this.unitOfWork.transaction((tx) => tx.pairingSessions.findById(pairingSessionId));
    }

    /** {@inheritDoc PairingApplication.confirm} */
    public async confirm(command: ConfirmPairingCommand): Promise<ImBinding> {
        await this.expireDue();
        const codeHash = await this.pairingCodes.hash(command.displayCode);
        const protectedIdentity = await this.identityProtector.protect(command.externalUserId);
        return this.unitOfWork.transaction(async (tx) => {
            const session = await tx.pairingSessions.findPendingByDisplayCodeHash(codeHash);
            if (session === undefined || session.expiresAt <= this.clock.now()) {
                throw new ImGatewayError('binding_not_found', 'Pairing session is invalid');
            }
            const account = await tx.channelAccounts.findById(command.channelAccountId);
            if (account === undefined || account.status !== 'active') {
                throw new ImGatewayError('binding_not_found', 'Channel account was not found');
            }
            if (session.allowedPlatforms !== undefined && !session.allowedPlatforms.includes(account.platform)) {
                throw new ImGatewayError('capability_not_supported', 'Platform is not allowed by the pairing session');
            }

            if (session.userId !== undefined && command.userId !== undefined && session.userId !== command.userId) {
                throw new ImGatewayError(
                    'invalid_transition',
                    'Pairing confirmation user does not match the pairing session',
                );
            }
            const userId = session.userId ?? command.userId;
            if (userId === undefined) {
                throw new ImGatewayError('binding_not_found', 'Pairing confirmation requires an internal userId');
            }

            let identity = await tx.identities.findByChannelAndHash(account.id, protectedIdentity.hash);
            const now = this.clock.now();
            if (identity !== undefined && identity.status !== 'active') {
                throw new ImGatewayError('binding_not_found', 'External identity is not active');
            }
            if (identity === undefined) {
                identity = {
                    id: this.ids.nextExternalIdentityId(),
                    channelAccountId: account.id,
                    externalUserIdCiphertext: protectedIdentity.ciphertext,
                    externalUserIdHash: protectedIdentity.hash,
                    ...(command.displayName === undefined ? {} : { displayName: command.displayName }),
                    status: 'active',
                    createdAt: now,
                    updatedAt: now,
                };
                await tx.identities.save(identity);
            }

            const binding: ImBinding = {
                id: this.ids.nextBindingId(),
                userId,
                deviceId: session.deviceId,
                externalIdentityId: identity.id,
                priority: 100,
                status: 'active',
                boundAt: now,
            };
            await tx.bindings.save(binding);
            await tx.pairingSessions.save({
                ...session,
                status: 'confirmed',
                confirmedAt: now,
            });
            return binding;
        });
    }

    /** {@inheritDoc PairingApplication.cancel} */
    public cancel(pairingSessionId: PairingSessionId): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            const session = await tx.pairingSessions.findById(pairingSessionId);
            if (session === undefined || session.status !== 'pending') return;
            await tx.pairingSessions.save({ ...session, status: 'cancelled' });
        });
    }

    /** {@inheritDoc PairingApplication.expireDue} */
    public expireDue(): Promise<number> {
        return this.unitOfWork.transaction(async (tx) => {
            const sessions = await tx.pairingSessions.findExpiredPairingSessions(this.clock.now());
            for (const session of sessions) {
                await tx.pairingSessions.save({ ...session, status: 'expired' });
            }
            return sessions.length;
        });
    }
}

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

/** 规范化入站事件幂等落库与状态推进的默认实现。 */
export class DefaultInboundEventApplication implements InboundEventApplication {
    /**
     * 创建入站事件应用服务。
     * @param unitOfWork 事务工作单元。
     * @param clock 业务时钟。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly clock: Clock,
    ) {}

    /** {@inheritDoc InboundEventApplication.recordIfNew} */
    public recordIfNew(event: NormalizedImEvent): Promise<'accepted' | 'duplicate'> {
        return this.unitOfWork.transaction(async (tx) => {
            const duplicate = await tx.inboundEvents.findByExternalEvent(event.channelAccountId, event.externalEventId);
            if (duplicate !== undefined) {
                if (duplicate.status !== 'failed') return 'duplicate';
                await tx.inboundEvents.save({
                    ...duplicate,
                    id: event.id,
                    payload: event.payload as unknown as JsonValue,
                    status: 'received',
                    occurredAt: event.occurredAt,
                    receivedAt: this.clock.now(),
                });
                return 'accepted';
            }
            await tx.inboundEvents.save({
                id: event.id,
                channelAccountId: event.channelAccountId,
                externalEventId: event.externalEventId,
                eventType: toInboundEventType(event.type),
                payload: event.payload as unknown as JsonValue,
                status: 'received',
                occurredAt: event.occurredAt,
                receivedAt: this.clock.now(),
            });
            return 'accepted';
        });
    }

    /** {@inheritDoc InboundEventApplication.markProcessing} */
    public markProcessing(eventId: NormalizedImEvent['id']): Promise<void> {
        return this.updateStatus(eventId, 'processing');
    }

    /** {@inheritDoc InboundEventApplication.markProcessed} */
    public markProcessed(eventId: NormalizedImEvent['id']): Promise<void> {
        return this.updateStatus(eventId, 'processed');
    }

    /** {@inheritDoc InboundEventApplication.markFailed} */
    public markFailed(eventId: NormalizedImEvent['id']): Promise<void> {
        return this.updateStatus(eventId, 'failed');
    }

    private updateStatus(
        eventId: NormalizedImEvent['id'],
        status: 'processing' | 'processed' | 'failed',
    ): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            const event = await tx.inboundEvents.findById(eventId);
            if (event === undefined) {
                throw new ImGatewayError('invalid_transition', 'Inbound event was not found');
            }
            await tx.inboundEvents.save({ ...event, status });
        });
    }
}

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

    /**
     * 分发一个已经完成平台归一化的入站事件。
     * @param event 规范化入站事件。
     * @returns 动作事件对应的设备命令；其他事件不返回值。
     */
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

/** 将设备通知意图转换为幂等投递记录的默认实现。 */
export class DefaultNotificationApplication implements NotificationApplication {
    /**
     * 创建通知受理服务。
     * @param unitOfWork 事务工作单元。
     * @param ids 标识生成器。
     * @param clock 业务时钟。
     * @param capabilities 渠道能力解析端口。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
        private readonly capabilities: ChannelCapabilityResolver,
    ) {}

    /** {@inheritDoc NotificationApplication.submitScheduleReceipt} */
    public submitScheduleReceipt(intent: ScheduleReceiptIntent): Promise<NotificationSubmission> {
        return this.createDeliveries({
            businessEventId: intent.eventId,
            correlationId: intent.correlationId,
            ...(intent.userId === undefined ? {} : { userId: intent.userId }),
            deviceId: intent.deviceId,
            kind: 'schedule_receipt',
            payload: intent as unknown as JsonValue,
        });
    }

    /** {@inheritDoc NotificationApplication.submitNotification} */
    public submitNotification(intent: NotificationIntent): Promise<NotificationSubmission> {
        return this.createDeliveries({
            businessEventId: intent.businessEventId,
            correlationId: intent.correlationId,
            userId: intent.recipient.userId,
            deviceId: intent.recipient.deviceId,
            kind: 'reminder_due',
            payload: intent as unknown as JsonValue,
            ...(intent.reminderType === 'strong'
                ? {
                      actionStream: {
                          reminderTriggerId: intent.reminderTriggerId,
                          expiresAt: this.clock.addMinutes(intent.triggerAt, DEFAULT_ACTION_WINDOW_MINUTES),
                      },
                  }
                : {}),
        });
    }

    /**
     * 创建通知交付。
     * @param input 交付输入。
     * @returns 通知交付。
     */
    private createDeliveries(input: {
        readonly businessEventId: ScheduleReceiptIntent['eventId'];
        readonly correlationId: ScheduleReceiptIntent['correlationId'];
        readonly userId?: UserId;
        readonly deviceId?: DeviceId;
        readonly kind: Delivery['kind'];
        readonly payload: JsonValue;
        readonly actionStream?: NonNullable<NotificationSubmission['actionStream']>;
    }): Promise<NotificationSubmission> {
        return this.unitOfWork.transaction(async (tx) => {
            const requestFingerprint = canonicalizeJson(input.payload);
            // 预占请求级幂等键：并发同键提交在此串行化，败者读到胜者的最终记录。
            const claim = await tx.intentSubmissions.createIfAbsent({
                businessEventId: input.businessEventId,
                kind: input.kind,
                requestFingerprint,
                submission: {
                    businessEventId: input.businessEventId,
                    status: 'accepted',
                    deliveries: [],
                },
                createdAt: this.clock.now(),
            });
            if (!claim.created) {
                if (claim.record.requestFingerprint !== requestFingerprint) {
                    throw new ImGatewayError(
                        'idempotency_conflict',
                        'Business event ID was already used with different contract content',
                    );
                }
                return claim.record.submission;
            }

            const bindings =
                input.userId === undefined
                    ? input.deviceId === undefined
                        ? []
                        : await tx.bindings.findActiveByDevice(input.deviceId)
                    : await tx.bindings.listActiveByUser(input.userId);
            const selected =
                input.deviceId === undefined
                    ? bindings
                    : bindings.filter(
                          (binding) => binding.deviceId === undefined || binding.deviceId === input.deviceId,
                      );
            const deliveries: NotificationSubmission['deliveries'][number][] = [];
            const now = this.clock.now();

            for (const binding of selected) {
                const existing = await tx.deliveries.findByBusinessKey(input.businessEventId, binding.id, input.kind);
                if (existing !== undefined) {
                    deliveries.push({
                        deliveryId: existing.id,
                        bindingId: binding.id,
                        status: 'pending',
                    });
                    continue;
                }
                const identity = await tx.identities.findById(binding.externalIdentityId);
                if (identity === undefined) continue;
                const account = await tx.channelAccounts.findById(identity.channelAccountId);
                if (account === undefined || account.status !== 'active') continue;
                const capability = await this.capabilities.resolve(account);
                const presentationType = choosePresentationType(capability, input.actionStream !== undefined);
                if (presentationType === undefined) continue;
                const candidate: Delivery = {
                    id: this.ids.nextDeliveryId(),
                    businessEventId: input.businessEventId,
                    correlationId: input.correlationId,
                    bindingId: binding.id,
                    channelAccountId: account.id,
                    kind: input.kind,
                    semanticPayload: input.payload,
                    presentationType,
                    status: 'pending',
                    ...(input.actionStream === undefined ? {} : { expiresAt: input.actionStream.expiresAt }),
                    createdAt: now,
                    updatedAt: now,
                };
                const { id: deliveryId, created } = await tx.deliveries.createIfAbsent(candidate);
                if (created) {
                    await tx.outbox.append({
                        id: this.ids.nextOutboxEventId(),
                        eventType: 'im.delivery.requested',
                        aggregateId: deliveryId,
                        payload: { deliveryId },
                        status: 'pending',
                        attempts: 0,
                        availableAt: now,
                        createdAt: now,
                    });
                }
                deliveries.push({
                    deliveryId,
                    bindingId: binding.id,
                    status: 'pending',
                });
            }

            const submission: NotificationSubmission = {
                businessEventId: input.businessEventId,
                status: 'accepted',
                deliveries,
                ...(input.actionStream === undefined || deliveries.length === 0
                    ? {}
                    : { actionStream: input.actionStream }),
            };
            // 仅 claim 持有者回填最终受理结果；占位记录与其在同一事务，崩溃即整体回滚。
            await tx.intentSubmissions.finalizeClaim({
                ...claim.record,
                submission,
            });
            return submission;
        });
    }
}

/** 投递详情查询与死信重试的默认实现。 */
export class DefaultDeliveryApplication implements DeliveryApplication {
    /**
     * 创建投递查询与恢复服务。
     * @param unitOfWork 事务工作单元。
     * @param ids 标识生成器。
     * @param clock 业务时钟。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
    ) {}

    /** {@inheritDoc DeliveryApplication.find} */
    public find(deliveryId: DeliveryId): Promise<DeliveryDetails | undefined> {
        return this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findById(deliveryId);
            if (delivery === undefined) return undefined;
            return {
                delivery,
                attempts: await tx.deliveries.listAttempts(deliveryId),
                receipts: await tx.deliveries.listReceipts(deliveryId),
            };
        });
    }

    /** {@inheritDoc DeliveryApplication.retryDeadLetter} */
    public retryDeadLetter(deliveryId: DeliveryId): Promise<Delivery> {
        return this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findById(deliveryId);
            if (delivery === undefined) {
                throw new ImGatewayError('delivery_not_found', 'Delivery was not found');
            }
            if (delivery.status !== 'dead_letter' && delivery.status !== 'permanent_failed') {
                throw new ImGatewayError(
                    'invalid_transition',
                    'Only dead-letter or permanently failed deliveries can be retried manually',
                );
            }
            const now = this.clock.now();
            const pending = {
                ...withoutDeliveryAttemptOutcome(delivery),
                status: 'pending' as const,
                updatedAt: now,
            };
            await tx.deliveries.save(pending);
            await tx.outbox.append({
                id: this.ids.nextOutboxEventId(),
                eventType: 'im.delivery.retry-requested',
                aggregateId: delivery.id,
                payload: { deliveryId: delivery.id },
                status: 'pending',
                attempts: 0,
                availableAt: now,
                createdAt: now,
            });
            return pending;
        });
    }
}

/** 消息渲染、发送及发送尝试状态推进的默认实现。 */
export class DefaultDeliveryDispatchApplication implements DeliveryDispatchApplication {
    /**
     * 创建投递调度服务。
     * @param unitOfWork 事务工作单元。
     * @param ids 标识生成器。
     * @param clock 业务时钟。
     * @param capabilities 渠道能力解析端口。
     * @param conversations 会话解析端口。
     * @param renderer 消息渲染端口。
     * @param channel IM 发送端口。
     * @param actionUi 动作入口服务。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
        private readonly capabilities: ChannelCapabilityResolver,
        private readonly conversations: ConversationResolverPort,
        private readonly renderer: DeliveryRendererPort,
        private readonly channel: ImChannelPort,
        private readonly actionUi: ActionUiApplication,
    ) {}

    /** {@inheritDoc DeliveryDispatchApplication.dispatch} */
    public async dispatch(deliveryId: DeliveryId): Promise<Delivery> {
        const claimTime = this.clock.now();
        const target = await this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.claimForDispatch(deliveryId, claimTime, DISPATCH_CLAIM_LEASE_SECONDS);
            if (delivery === undefined) {
                const missing = await tx.deliveries.findById(deliveryId);
                if (missing === undefined) {
                    throw new ImGatewayError('delivery_not_found', 'Delivery was not found');
                }
                throw new ImGatewayError(
                    'invalid_transition',
                    'Only pending or retryable deliveries can be dispatched',
                );
            }
            const binding = await tx.bindings.findById(delivery.bindingId);
            const identity =
                binding === undefined ? undefined : await tx.identities.findById(binding.externalIdentityId);
            const account = await tx.channelAccounts.findById(delivery.channelAccountId);
            if (
                binding === undefined ||
                binding.status !== 'active' ||
                identity === undefined ||
                identity.status !== 'active' ||
                account === undefined ||
                account.status !== 'active'
            ) {
                throw new ImGatewayError('binding_not_found', 'Delivery target is incomplete');
            }
            return { delivery, identity, account };
        });
        const claimToken = target.delivery.claimToken;
        if (claimToken === undefined) {
            throw new Error('Claimed delivery is missing a claim token');
        }

        let preSend: { readonly renderedPayload: JsonValue; readonly conversation: ConversationRef };
        try {
            const capabilities = await this.capabilities.resolve(target.account);
            const actionToken =
                target.delivery.expiresAt === undefined ? undefined : await this.actionUi.issue(target.delivery.id);
            const renderedPayload = await this.renderer.render(
                target.delivery,
                target.account,
                capabilities,
                actionToken === undefined ? {} : { actionToken },
            );
            const conversation = await this.conversations.resolveDirect(target.identity);
            preSend = { renderedPayload, conversation };
        } catch {
            return this.recordPreSendFailure(deliveryId, target.delivery, claimToken);
        }

        const gate = await this.unitOfWork.transaction(async (tx) => {
            const renewal: Delivery = {
                ...withoutDeliveryAttemptOutcome(target.delivery),
                status: 'sending',
                claimedAt: this.clock.now(),
                claimToken,
                updatedAt: this.clock.now(),
            };
            const owned = await tx.deliveries.saveIfClaimed(renewal, claimToken);
            if (owned === undefined) {
                return {
                    owned: false as const,
                    delivery: (await tx.deliveries.findById(deliveryId)) ?? target.delivery,
                };
            }
            const attemptNo = await tx.deliveries.nextAttemptNo(deliveryId);
            const attempt = {
                id: this.ids.nextDeliveryAttemptId(),
                deliveryId,
                attemptNo,
                requestId: this.ids.nextRequestId(),
                renderedPayload: preSend.renderedPayload,
                status: 'sending' as const,
                startedAt: this.clock.now(),
            };
            await tx.deliveries.saveAttempt(attempt);
            return { owned: true as const, delivery: owned, attempt };
        });
        if (!gate.owned) return gate.delivery;
        const attempt = gate.attempt;

        let acceptance: ImSendAcceptance;
        try {
            acceptance = await this.channel.send({
                delivery: target.delivery,
                conversation: preSend.conversation,
                content: preSend.renderedPayload,
            });
        } catch {
            acceptance = {
                accepted: false,
                retryable: true,
                errorCode: 'channel_send_exception',
            } as const;
        }
        const completionTime = this.clock.now();
        const status = acceptance.accepted
            ? 'accepted'
            : acceptance.retryable === true
              ? 'retryable_failed'
              : 'permanent_failed';
        const terminal: Delivery = {
            ...withoutDeliveryAttemptOutcome(target.delivery),
            status,
            ...(status !== 'accepted' || acceptance.platformMessageId === undefined
                ? {}
                : { externalMessageId: acceptance.platformMessageId }),
            ...(acceptance.errorCode === undefined ? {} : { lastErrorCode: acceptance.errorCode }),
            updatedAt: completionTime,
        };
        return this.unitOfWork.transaction(async (tx) => {
            const written = await tx.deliveries.saveIfClaimed(terminal, claimToken);
            if (written === undefined) {
                // 发送期间失去所有权（超时被重领）：放弃 attempt 与 outbox 回写，避免覆盖新 owner。
                return (await tx.deliveries.findById(deliveryId)) ?? terminal;
            }
            await tx.deliveries.saveAttempt({
                ...attempt,
                status,
                ...(acceptance.platformMessageId === undefined
                    ? {}
                    : { platformMessageId: acceptance.platformMessageId }),
                ...(acceptance.errorCode === undefined ? {} : { errorCode: acceptance.errorCode }),
                completedAt: completionTime,
            });
            if (status === 'retryable_failed') {
                await tx.outbox.append({
                    id: this.ids.nextOutboxEventId(),
                    eventType: 'im.delivery.retry-scheduled',
                    aggregateId: deliveryId,
                    payload: { deliveryId },
                    status: 'pending',
                    attempts: attempt.attemptNo,
                    availableAt: this.clock.addMinutes(completionTime, 1),
                    createdAt: completionTime,
                });
            }
            return written;
        });
    }

    /**
     * 将发送前步骤（能力解析/动作签发/渲染/会话解析）的异常转为可重试失败并计划重试。
     * @param deliveryId 投递标识。
     * @param delivery 本次领取的投递。
     * @param claimToken 本次派发的所有权令牌。
     * @returns 写入后的投递；所有权已丢失时返回当前投递。
     */
    private async recordPreSendFailure(
        deliveryId: DeliveryId,
        delivery: Delivery,
        claimToken: string,
    ): Promise<Delivery> {
        const now = this.clock.now();
        return this.unitOfWork.transaction(async (tx) => {
            const failed: Delivery = {
                ...withoutDeliveryAttemptOutcome(delivery),
                status: 'retryable_failed',
                lastErrorCode: 'pre_send_exception',
                updatedAt: now,
            };
            const written = await tx.deliveries.saveIfClaimed(failed, claimToken);
            if (written === undefined) {
                return (await tx.deliveries.findById(deliveryId)) ?? failed;
            }
            const attemptNo = await tx.deliveries.nextAttemptNo(deliveryId);
            await tx.deliveries.saveAttempt({
                id: this.ids.nextDeliveryAttemptId(),
                deliveryId,
                attemptNo,
                requestId: this.ids.nextRequestId(),
                renderedPayload: {},
                status: 'retryable_failed',
                errorCode: 'pre_send_exception',
                startedAt: now,
                completedAt: now,
            });
            await tx.outbox.append({
                id: this.ids.nextOutboxEventId(),
                eventType: 'im.delivery.retry-scheduled',
                aggregateId: deliveryId,
                payload: { deliveryId },
                status: 'pending',
                attempts: attemptNo,
                availableAt: this.clock.addMinutes(now, 1),
                createdAt: now,
            });
            return written;
        });
    }

    /** {@inheritDoc DeliveryDispatchApplication.markDeadLetter} */
    public markDeadLetter(deliveryId: DeliveryId): Promise<Delivery> {
        return this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findById(deliveryId);
            if (delivery === undefined) {
                throw new ImGatewayError('delivery_not_found', 'Delivery was not found');
            }
            if (delivery.status !== 'retryable_failed' && delivery.status !== 'permanent_failed') {
                throw new ImGatewayError(
                    'invalid_transition',
                    'Only failed deliveries can enter the dead-letter state',
                );
            }
            const deadLetter: Delivery = {
                ...delivery,
                status: 'dead_letter',
                updatedAt: this.clock.now(),
            };
            await tx.deliveries.save(deadLetter);
            return deadLetter;
        });
    }
}

/** 平台投递回执去重与终态归并的默认实现。 */
export class DefaultReceiptApplication implements ReceiptApplication {
    /**
     * 创建投递回执服务。
     * @param unitOfWork 事务工作单元。
     * @param ids 标识生成器。
     * @param clock 业务时钟。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
    ) {}

    /** {@inheritDoc ReceiptApplication.record} */
    public record(receipt: NormalizedDeliveryReceipt): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            if ((await tx.deliveries.findReceiptByDedupeKey(receipt.dedupeKey)) !== undefined) {
                return;
            }
            const delivery = await tx.deliveries.findByExternalMessage(
                receipt.channelAccountId,
                receipt.externalMessageId,
            );
            if (delivery === undefined) {
                throw new ImGatewayError('delivery_not_found', 'Delivery was not found for the platform message');
            }
            const attempts = await tx.deliveries.listAttempts(delivery.id);
            const matchingAttempts = attempts.filter(
                (attempt) => attempt.platformMessageId === receipt.externalMessageId,
            );
            const receiptAttempt =
                receipt.attemptId === undefined
                    ? matchingAttempts.length === 1
                        ? matchingAttempts[0]
                        : undefined
                    : matchingAttempts.find((attempt) => attempt.id === receipt.attemptId);
            if (receipt.attemptId !== undefined && receiptAttempt === undefined) {
                throw new ImGatewayError('invalid_contract', 'Receipt attempt does not match its platform message');
            }
            const detail =
                receipt.retryable === true
                    ? { ...(isJsonObject(receipt.detail) ? receipt.detail : {}), retryable: true }
                    : receipt.detail;
            await tx.deliveries.saveReceipt({
                id: this.ids.nextDeliveryReceiptId(),
                deliveryId: delivery.id,
                ...(receipt.attemptId === undefined ? {} : { attemptId: receipt.attemptId }),
                stage: receipt.stage,
                dedupeKey: receipt.dedupeKey,
                externalEventId: receipt.externalEventId,
                ...(detail === undefined ? {} : { detail }),
                occurredAt: receipt.occurredAt,
                receivedAt: this.clock.now(),
            });
            const currentAttempt = attempts.at(-1);
            // 旧尝试或无法唯一关联的回执只保留审计记录，不能推进当前投递。
            if (
                receipt.externalMessageId !== delivery.externalMessageId ||
                currentAttempt?.status !== 'accepted' ||
                receiptAttempt?.id !== currentAttempt.id
            ) {
                return;
            }
            const status = advanceDeliveryStatus(delivery.status, receipt);
            if (status !== delivery.status) {
                await tx.deliveries.save({
                    ...delivery,
                    status,
                    updatedAt: this.clock.now(),
                });
            }
        });
    }
}

/** 提醒动作准备、派发、回放和结果处理的默认实现。 */
export class DefaultActionApplication implements ActionApplication {
    /**
     * 创建提醒动作应用服务。
     * @param unitOfWork 事务工作单元。
     * @param stream 动作命令流端口。
     * @param ids 标识生成器。
     * @param clock 业务时钟。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly stream: ActionCommandStreamPort,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
    ) {}

    /** {@inheritDoc ActionApplication.prepareToken} */
    public prepareToken(deliveryId: DeliveryId): Promise<ActionTokenClaims> {
        return this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findById(deliveryId);
            if (
                delivery === undefined ||
                delivery.expiresAt === undefined ||
                delivery.expiresAt <= this.clock.now() ||
                readStrongReminderMetadata(delivery.semanticPayload) === undefined
            ) {
                throw new ImGatewayError('action_expired', 'Delivery has no active strong-reminder action window');
            }
            return {
                // Production implementations may back this stable mapping with UUIDv5
                // or a persisted token-preparation row.
                actionId: this.ids.actionIdForDelivery(delivery.id),
                deliveryId: delivery.id,
                expiresAt: delivery.expiresAt,
            };
        });
    }

    /** {@inheritDoc ActionApplication.inspectPrepared} */
    public inspectPrepared(claims: ActionTokenClaims): Promise<ActionUiView> {
        return this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findById(claims.deliveryId);
            const metadata = delivery === undefined ? undefined : readStrongReminderMetadata(delivery.semanticPayload);
            if (
                delivery === undefined ||
                metadata === undefined ||
                delivery.expiresAt !== claims.expiresAt ||
                claims.expiresAt <= this.clock.now()
            ) {
                throw new ImGatewayError('action_expired', 'Action UI token has expired');
            }
            return {
                actionId: claims.actionId,
                actions: metadata.options.map((option) => option.type),
                options: metadata.options.map((option) => ({
                    action: option.type,
                    label: option.label,
                    ...(option.params === undefined ? {} : { params: option.params }),
                })),
                expiresAt: claims.expiresAt,
            };
        });
    }

    /** {@inheritDoc ActionApplication.triggerPrepared} */
    public async triggerPrepared(command: TriggerPreparedActionCommand): Promise<ReminderActionCommand> {
        const actionParams = validateReminderActionParams(command.actionType, command.actionParams);
        const prepared = await this.unitOfWork.transaction(async (tx) => {
            const existing = await tx.actions.findById(command.claims.actionId);
            if (existing !== undefined) {
                if (
                    existing.deliveryId !== command.claims.deliveryId ||
                    existing.actionType !== command.actionType ||
                    !sameActionParams(existing.actionParams, actionParams) ||
                    (command.actualIdentityId !== undefined && command.actualIdentityId !== existing.expectedIdentityId)
                ) {
                    throw new ImGatewayError('action_not_found', 'Action token was already used for another action');
                }
                return { command: toCommand(existing), shouldDispatch: false };
            }
            const duplicateKey = await tx.actions.findByActionKeyHash(command.actionKeyHash);
            if (duplicateKey !== undefined) {
                if (duplicateKey.id !== command.claims.actionId) {
                    throw new ImGatewayError(
                        'action_not_found',
                        'Action token fingerprint is already bound to another Action',
                    );
                }
                return { command: toCommand(duplicateKey), shouldDispatch: false };
            }

            const delivery = await tx.deliveries.findById(command.claims.deliveryId);
            const metadata = delivery === undefined ? undefined : readStrongReminderMetadata(delivery.semanticPayload);
            const binding = delivery === undefined ? undefined : await tx.bindings.findById(delivery.bindingId);
            if (
                delivery === undefined ||
                binding === undefined ||
                binding.status !== 'active' ||
                (command.actualIdentityId !== undefined && command.actualIdentityId !== binding.externalIdentityId) ||
                metadata === undefined ||
                !hasApprovedActionOption(metadata.options, command.actionType, actionParams) ||
                delivery.expiresAt !== command.claims.expiresAt ||
                command.claims.expiresAt <= this.clock.now()
            ) {
                throw new ImGatewayError('action_expired', 'Action token does not match an active delivery');
            }
            const now = this.clock.now();
            const action: ImAction = {
                id: command.claims.actionId,
                operationId: this.ids.nextOperationId(),
                correlationId: delivery.correlationId,
                deliveryId: delivery.id,
                actorBindingId: binding.id,
                deviceId: metadata.deviceId,
                reminderTriggerId: metadata.reminderTriggerId,
                actionType: command.actionType,
                ...(actionParams === undefined ? {} : { actionParams }),
                actionKeyHash: command.actionKeyHash,
                expectedIdentityId: binding.externalIdentityId,
                actualIdentityId: command.actualIdentityId ?? binding.externalIdentityId,
                status: 'pending',
                expiresAt: command.claims.expiresAt,
                createdAt: now,
                updatedAt: now,
            };
            await tx.actions.save(action);
            return { command: toCommand(action), shouldDispatch: true };
        });
        if (prepared.shouldDispatch) await this.dispatch(prepared.command);
        return prepared.command;
    }

    /** {@inheritDoc ActionApplication.recordResult} */
    public recordResult(commandId: ActionId, deviceId: DeviceId, result: ReminderActionResult): Promise<ImAction> {
        return this.recordResultAndClose(commandId, deviceId, result);
    }

    private async recordResultAndClose(
        commandId: ActionId,
        deviceId: DeviceId,
        result: ReminderActionResult,
    ): Promise<ImAction> {
        const updated = await this.unitOfWork.transaction(async (tx) => {
            const action = await tx.actions.findById(commandId);
            if (action === undefined) {
                throw new ImGatewayError('action_not_found', 'Action was not found');
            }
            if (
                action.operationId !== result.operationId ||
                action.deviceId !== deviceId ||
                action.reminderTriggerId !== result.reminderTriggerId
            ) {
                throw new ImGatewayError('invalid_transition', 'Action result does not match command scope');
            }
            if (
                result.status === 'succeeded' &&
                ((action.actionType === 'snooze' && result.nextTriggerAt === undefined) ||
                    (action.actionType === 'acknowledge' && result.nextTriggerAt !== undefined))
            ) {
                throw new ImGatewayError(
                    'invalid_transition',
                    'Action result nextTriggerAt does not match its action type',
                );
            }
            if (action.status === 'succeeded' || action.status === 'failed' || action.status === 'expired') {
                if (action.result?.status === result.status) return action;
                throw new ImGatewayError('invalid_transition', 'A terminal Action result cannot be overwritten');
            }
            const status: ActionStatus = result.status === 'retryable_failed' ? 'pending' : result.status;
            const updated: ImAction = {
                ...action,
                status,
                result,
                updatedAt: this.clock.now(),
            };
            await tx.actions.save(updated);
            return updated;
        });
        if (result.status === 'retryable_failed') {
            await this.dispatch(toCommand(updated));
        } else {
            await this.stream.close(updated.id);
        }
        return updated;
    }

    /** {@inheritDoc ActionApplication.expireDue} */
    public async expireDue(): Promise<number> {
        const expired = await this.unitOfWork.transaction(async (tx) => {
            const actions = await tx.actions.findExpiredActions(this.clock.now());
            for (const action of actions) {
                await tx.actions.save({
                    ...action,
                    status: 'expired',
                    updatedAt: this.clock.now(),
                });
            }
            return actions;
        });
        for (const action of expired) await this.stream.close(action.id);
        return expired.length;
    }

    /** {@inheritDoc ActionApplication.resolveActionWindow} */
    public resolveActionWindow(deviceId: DeviceId, reminderTriggerId: ReminderTriggerId): Promise<IsoDateTime> {
        return this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findActiveActionWindow(deviceId, reminderTriggerId, this.clock.now());
            if (delivery?.expiresAt === undefined) {
                throw new ImGatewayError('action_expired', 'No active strong-reminder action window was found');
            }
            return delivery.expiresAt;
        });
    }

    /** {@inheritDoc ActionApplication.markProcessing} */
    public markProcessing(actionId: ActionId, deviceId: DeviceId, reminderTriggerId: ReminderTriggerId): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            const action = await tx.actions.findById(actionId);
            if (
                action !== undefined &&
                action.deviceId === deviceId &&
                action.reminderTriggerId === reminderTriggerId &&
                action.status === 'processing'
            ) {
                return;
            }
            if (
                action === undefined ||
                action.deviceId !== deviceId ||
                action.reminderTriggerId !== reminderTriggerId ||
                action.status !== 'dispatched'
            ) {
                throw new ImGatewayError('invalid_transition', 'Action cannot enter processing for this stream');
            }
            await tx.actions.save({
                ...action,
                status: 'processing',
                updatedAt: this.clock.now(),
            });
        });
    }

    /** {@inheritDoc ActionApplication.find} */
    public find(actionId: ActionId): Promise<ImAction | undefined> {
        return this.unitOfWork.transaction((tx) => tx.actions.findById(actionId));
    }

    /** {@inheritDoc ActionApplication.findByOperationId} */
    public findByOperationId(operationId: OperationId): Promise<ImAction | undefined> {
        return this.unitOfWork.transaction((tx) => tx.actions.findByOperationId(operationId));
    }

    /** {@inheritDoc ActionApplication.replayPending} */
    public replayPending(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        after?: ActionId,
    ): Promise<readonly ReminderActionCommand[]> {
        return this.unitOfWork.transaction(async (tx) => {
            const actions = await tx.actions.findPendingByDeviceAndTrigger(
                deviceId,
                reminderTriggerId,
                this.clock.now(),
            );
            // Last-Event-ID 只描述传输进度；业务结果返回前，任何命令都不能被游标排除。
            void after;
            const replay = actions;
            for (const action of replay) {
                if (action.status === 'pending') {
                    await tx.actions.save({
                        ...action,
                        status: 'dispatched',
                        dispatchedAt: this.clock.now(),
                        updatedAt: this.clock.now(),
                    });
                }
            }
            return replay.map(toCommand);
        });
    }

    private async dispatch(command: ReminderActionCommand): Promise<void> {
        await this.unitOfWork.transaction(async (tx) => {
            const action = await tx.actions.findById(command.commandId);
            if (action === undefined) return;
            await tx.actions.save({
                ...action,
                status: 'dispatched',
                dispatchedAt: this.clock.now(),
                updatedAt: this.clock.now(),
            });
        });
        await this.stream.publish(command);
    }
}

/** 短期动作令牌签发、展示与执行的默认实现。 */
export class DefaultActionUiApplication implements ActionUiApplication {
    /**
     * 创建动作页面应用服务。
     * @param tokens 动作令牌端口。
     * @param actions 提醒动作服务。
     * @param clock 业务时钟。
     */
    public constructor(
        private readonly tokens: ActionTokenPort,
        private readonly actions: ActionApplication,
        private readonly clock: Clock,
    ) {}

    /** {@inheritDoc ActionUiApplication.issue} */
    public async issue(deliveryId: DeliveryId): Promise<string> {
        return this.tokens.issue(await this.actions.prepareToken(deliveryId));
    }

    /** {@inheritDoc ActionUiApplication.show} */
    public async show(token: string): Promise<ActionUiView> {
        const claims = await this.tokens.verify(token);
        if (claims.expiresAt <= this.clock.now()) {
            throw new ImGatewayError('action_expired', 'Action UI token has expired');
        }
        return this.actions.inspectPrepared(claims);
    }

    /** {@inheritDoc ActionUiApplication.execute} */
    public async execute(
        input: Parameters<ActionUiApplication['execute']>[0],
        context?: Parameters<ActionUiApplication['execute']>[1],
    ): Promise<ReminderActionCommand> {
        const claims = await this.tokens.verify(input.token);
        if (claims.expiresAt <= this.clock.now()) {
            throw new ImGatewayError('action_expired', 'Action UI token has expired');
        }
        return this.actions.triggerPrepared({
            claims,
            actionType: input.action,
            actionKeyHash: await this.tokens.fingerprint(input.token),
            ...(context?.actualIdentityId === undefined ? {} : { actualIdentityId: context.actualIdentityId }),
            ...(input.params === undefined ? {} : { actionParams: input.params }),
        });
    }
}

/**
 * 将 ImAction 转换为 ReminderActionCommand 的辅助函数。
 * @param action 要转换的 ImAction 动作。
 * @returns 转换后的 ReminderActionCommand 命令。
 */
function toCommand(action: ImAction): ReminderActionCommand {
    const minutes =
        typeof action.actionParams === 'object' &&
        action.actionParams !== null &&
        !Array.isArray(action.actionParams) &&
        typeof action.actionParams.minutes === 'number'
            ? action.actionParams.minutes
            : undefined;
    return {
        schemaVersion: DEVICE_CONTRACT_VERSION,
        commandId: action.id,
        operationId: action.operationId,
        correlationId: action.correlationId,
        deviceId: action.deviceId,
        actorBindingId: action.actorBindingId,
        reminderTriggerId: action.reminderTriggerId,
        action: action.actionType,
        ...(minutes === undefined ? {} : { params: { minutes } }),
        occurredAt: action.createdAt,
        expiresAt: action.expiresAt,
    };
}

/**
 * 将 InboundEventType 转换为归一化的入站事件类型。
 * @param type 要转换的类型。
 * @returns 归一化的入站事件类型。
 */
function toInboundEventType(type: NormalizedImEvent['type']): NormalizedImEvent['type'] {
    return type;
}

/**
 * 选择合适的呈现类型。
 * @param capabilities 通道能力解析器的返回能力。
 * @param hasActions 是否有动作。
 * @returns 首选呈现类型；渠道无法主动发送或无法承载动作时返回 undefined。
 */
function choosePresentationType(
    capabilities: Awaited<ReturnType<ChannelCapabilityResolver['resolve']>>,
    hasActions: boolean,
): PresentationType | undefined {
    if (!capabilities.proactiveMessage) return undefined;
    if (hasActions && capabilities.nativeAction && capabilities.presentationTypes.includes('native_card')) {
        return 'native_card';
    }
    if (hasActions && !capabilities.actionUi) return undefined;
    if (capabilities.presentationTypes.includes('template')) return 'template';
    if (capabilities.presentationTypes.includes('rich_text')) return 'rich_text';
    if (capabilities.presentationTypes.includes('text_with_action_ui')) return 'text_with_action_ui';
    return undefined;
}

/**
 * 读取强提醒元数据。
 * @param payload 元数据的 JSON 值。
 * @returns 强提醒元数据，不符合结构时返回 undefined。
 */
function readStrongReminderMetadata(payload: JsonValue):
    | {
          readonly deviceId: DeviceId;
          readonly reminderTriggerId: ReminderTriggerId;
          readonly options: readonly NotificationActionOption[];
      }
    | undefined {
    if (
        typeof payload !== 'object' ||
        payload === null ||
        Array.isArray(payload) ||
        payload.reminderType !== 'strong' ||
        typeof payload.reminderTriggerId !== 'string' ||
        typeof payload.recipient !== 'object' ||
        payload.recipient === null ||
        Array.isArray(payload.recipient) ||
        typeof payload.recipient.deviceId !== 'string' ||
        !Array.isArray(payload.actions)
    ) {
        return undefined;
    }
    const options: NotificationActionOption[] = [];
    for (const candidate of payload.actions) {
        if (
            typeof candidate !== 'object' ||
            candidate === null ||
            Array.isArray(candidate) ||
            (candidate.type !== 'acknowledge' && candidate.type !== 'snooze')
        ) {
            continue;
        }
        if (typeof candidate.label !== 'string' || candidate.label.trim() === '') continue;
        const params =
            candidate.params !== undefined &&
            typeof candidate.params === 'object' &&
            candidate.params !== null &&
            !Array.isArray(candidate.params) &&
            typeof candidate.params.minutes === 'number' &&
            Number.isInteger(candidate.params.minutes) &&
            candidate.params.minutes > 0
                ? { minutes: candidate.params.minutes }
                : undefined;
        if (candidate.type === 'snooze' && params === undefined) continue;
        options.push({
            kind: 'command',
            type: candidate.type,
            label: candidate.label,
            ...(params === undefined ? {} : { params }),
        });
    }
    if (options.length === 0) return undefined;
    return {
        deviceId: payload.recipient.deviceId as DeviceId,
        reminderTriggerId: payload.reminderTriggerId as ReminderTriggerId,
        options,
    };
}

function hasApprovedActionOption(
    options: readonly NotificationActionOption[],
    action: ReminderActionCommand['action'],
    params: JsonValue | undefined,
): boolean {
    return options.some(
        (option) =>
            option.type === action &&
            (option.params === undefined
                ? params === undefined
                : isJsonObject(params) && params.minutes === option.params.minutes),
    );
}

function sameActionParams(left: JsonValue | undefined, right: JsonValue | undefined): boolean {
    if (left === undefined || right === undefined) return left === right;
    return canonicalizeJson(left) === canonicalizeJson(right);
}

/**
 * 验证提醒动作参数。
 * @param action 动作类型。
 * @param params 动作参数。
 * @returns 验证后的参数。
 */
function validateReminderActionParams(
    action: ReminderActionCommand['action'],
    params: JsonValue | undefined,
): JsonValue | undefined {
    if (action === 'acknowledge') {
        if (params !== undefined) {
            throw new ImGatewayError('invalid_transition', 'acknowledge does not accept action params');
        }
        return undefined;
    }
    if (
        typeof params !== 'object' ||
        params === null ||
        Array.isArray(params) ||
        typeof params.minutes !== 'number' ||
        !Number.isInteger(params.minutes) ||
        params.minutes <= 0
    ) {
        throw new ImGatewayError('invalid_transition', 'snooze requires a positive integer params.minutes');
    }
    return { minutes: params.minutes };
}

/**
 * 返回清除上一次发送结果与派发所有权后的投递；新尝试不继承旧消息标识、错误码或 claim 归属。
 * @param delivery 原投递。
 * @returns 无 externalMessageId、lastErrorCode、claimedAt 与 claimToken 的投递。
 */
function withoutDeliveryAttemptOutcome(delivery: Delivery): Delivery {
    const cleared = { ...delivery };
    delete cleared.lastErrorCode;
    delete cleared.externalMessageId;
    delete cleared.claimedAt;
    delete cleared.claimToken;
    return cleared;
}

function isJsonObject(value: JsonValue | undefined): value is { readonly [key: string]: JsonValue } {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * 推进交付状态。
 * @param current 当前状态。
 * @param receipt 接收状态。
 * @returns 推进后的状态。
 */
function advanceDeliveryStatus(current: DeliveryStatus, receipt: NormalizedDeliveryReceipt): DeliveryStatus {
    if (current === 'delivered') return current;
    if (receipt.stage === 'delivered') return 'delivered';
    if (current === 'dead_letter' || current === 'permanent_failed') return current;
    return receipt.retryable === true ? 'retryable_failed' : 'permanent_failed';
}
