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
    type NotificationIntent,
    type NotificationSubmission,
    type ReminderActionCommand,
    type ReminderActionResult,
    type ScheduleReceiptIntent,
} from '../contracts/device-gateway.js';
import type { NormalizedDeliveryReceipt, NormalizedImEvent } from '../contracts/platform-events.js';
import type {
    ActionStatus,
    ChannelAccount,
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

export class DefaultChannelAccountApplication implements ChannelAccountApplication {
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
        private readonly healthPort: ChannelHealthPort,
    ) {}

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

    public find(channelAccountId: ChannelAccountId): Promise<ChannelAccount | undefined> {
        return this.unitOfWork.transaction((tx) => tx.channelAccounts.findById(channelAccountId));
    }

    public async health(channelAccountId: ChannelAccountId): Promise<ChannelHealth> {
        const account = await this.find(channelAccountId);
        if (account === undefined) {
            throw new ImGatewayError('binding_not_found', 'Channel account was not found');
        }
        return this.healthPort.check(account);
    }
}

export class DefaultPairingApplication implements PairingApplication {
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
        private readonly pairingCodes: PairingCodePort,
        private readonly identityProtector: ExternalIdentityProtector,
    ) {}

    public async create(command: CreatePairingSessionCommand): Promise<CreatedPairingSession> {
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

    public async find(pairingSessionId: PairingSessionId): Promise<PairingSession | undefined> {
        await this.expireDue();
        return this.unitOfWork.transaction((tx) => tx.pairingSessions.findById(pairingSessionId));
    }

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
            if (account === undefined) {
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

    public cancel(pairingSessionId: PairingSessionId): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            const session = await tx.pairingSessions.findById(pairingSessionId);
            if (session === undefined || session.status !== 'pending') return;
            await tx.pairingSessions.save({ ...session, status: 'cancelled' });
        });
    }

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

export class DefaultBindingApplication implements BindingApplication {
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly clock: Clock,
    ) {}

    public list(userId: UserId): Promise<readonly ImBinding[]> {
        return this.unitOfWork.transaction((tx) => tx.bindings.listActiveByUser(userId));
    }

    public unbind(bindingId: BindingId): Promise<void> {
        return this.changeStatus(bindingId, 'unbound');
    }

    public revoke(bindingId: BindingId): Promise<void> {
        return this.changeStatus(bindingId, 'revoked');
    }

    public findActiveByExternalIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined> {
        return this.unitOfWork.transaction((tx) => tx.bindings.findActiveByIdentity(externalIdentityId));
    }

    private changeStatus(bindingId: BindingId, status: 'unbound' | 'revoked'): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            const binding = await tx.bindings.findById(bindingId);
            if (binding === undefined) {
                throw new ImGatewayError('binding_not_found', 'Binding was not found');
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

export class DefaultInboundEventApplication implements InboundEventApplication {
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly clock: Clock,
    ) {}

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

    public markProcessing(eventId: NormalizedImEvent['id']): Promise<void> {
        return this.updateStatus(eventId, 'processing');
    }

    public markProcessed(eventId: NormalizedImEvent['id']): Promise<void> {
        return this.updateStatus(eventId, 'processed');
    }

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

export class DefaultPlatformEventApplication implements PlatformEventApplication {
    public constructor(
        private readonly inboundEvents: InboundEventApplication,
        private readonly pairing: PairingApplication,
        private readonly receipts: ReceiptApplication,
        private readonly actionUi: ActionUiApplication,
    ) {}

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

export class DefaultNotificationApplication implements NotificationApplication {
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
        private readonly capabilities: ChannelCapabilityResolver,
    ) {}

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
            const existingSubmission = await tx.intentSubmissions.findByBusinessKey(input.businessEventId, input.kind);
            if (existingSubmission !== undefined) {
                if (existingSubmission.requestFingerprint !== requestFingerprint) {
                    throw new ImGatewayError(
                        'idempotency_conflict',
                        'Business event ID was already used with different contract content',
                    );
                }
                return existingSubmission.submission;
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
                const delivery: Delivery = {
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
                await tx.deliveries.save(delivery);
                await tx.outbox.append({
                    id: this.ids.nextOutboxEventId(),
                    eventType: 'im.delivery.requested',
                    aggregateId: delivery.id,
                    payload: { deliveryId: delivery.id },
                    status: 'pending',
                    attempts: 0,
                    availableAt: now,
                    createdAt: now,
                });
                deliveries.push({
                    deliveryId: delivery.id,
                    bindingId: binding.id,
                    status: 'pending',
                });
            }

            const submission: NotificationSubmission = {
                businessEventId: input.businessEventId,
                status: 'accepted',
                deliveries,
                ...(input.actionStream === undefined ? {} : { actionStream: input.actionStream }),
            };
            await tx.intentSubmissions.save({
                businessEventId: input.businessEventId,
                kind: input.kind,
                requestFingerprint,
                submission,
                createdAt: now,
            });
            return submission;
        });
    }
}

export class DefaultDeliveryApplication implements DeliveryApplication {
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
    ) {}

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

    public retryDeadLetter(deliveryId: DeliveryId): Promise<Delivery> {
        return this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findById(deliveryId);
            if (delivery === undefined) {
                throw new ImGatewayError('delivery_not_found', 'Delivery was not found');
            }
            if (delivery.status !== 'dead_letter' && delivery.status !== 'permanent_failed') {
                throw new ImGatewayError('invalid_transition', 'Only dead-letter deliveries can be retried manually');
            }
            const now = this.clock.now();
            const pending: Delivery = { ...delivery, status: 'pending', updatedAt: now };
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

export class DefaultDeliveryDispatchApplication implements DeliveryDispatchApplication {
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

    public async dispatch(deliveryId: DeliveryId): Promise<Delivery> {
        const target = await this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findById(deliveryId);
            if (delivery === undefined) {
                throw new ImGatewayError('delivery_not_found', 'Delivery was not found');
            }
            if (delivery.status !== 'pending' && delivery.status !== 'retryable_failed') {
                throw new ImGatewayError(
                    'invalid_transition',
                    'Only pending or retryable deliveries can be dispatched',
                );
            }
            const binding = await tx.bindings.findById(delivery.bindingId);
            const identity =
                binding === undefined ? undefined : await tx.identities.findById(binding.externalIdentityId);
            const account = await tx.channelAccounts.findById(delivery.channelAccountId);
            if (binding === undefined || identity === undefined || account === undefined) {
                throw new ImGatewayError('binding_not_found', 'Delivery target is incomplete');
            }
            return { delivery, identity, account };
        });

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
        const attempt = await this.unitOfWork.transaction(async (tx) => {
            const attemptNo = await tx.deliveries.nextAttemptNo(deliveryId);
            if (target.delivery.status === 'retryable_failed') {
                await tx.deliveries.save({
                    ...target.delivery,
                    status: 'pending',
                    updatedAt: this.clock.now(),
                });
            }
            const started = {
                id: this.ids.nextDeliveryAttemptId(),
                deliveryId,
                attemptNo,
                requestId: this.ids.nextRequestId(),
                renderedPayload,
                status: 'sending' as const,
                startedAt: this.clock.now(),
            };
            await tx.deliveries.saveAttempt(started);
            await tx.deliveries.save({
                ...target.delivery,
                status: 'sending',
                updatedAt: this.clock.now(),
            });
            return started;
        });

        let acceptance: ImSendAcceptance;
        try {
            acceptance = await this.channel.send({
                delivery: target.delivery,
                conversation,
                content: renderedPayload,
            });
        } catch {
            acceptance = {
                accepted: false,
                retryable: true,
                errorCode: 'channel_send_exception',
            } as const;
        }
        return this.unitOfWork.transaction(async (tx) => {
            const status = acceptance.accepted
                ? 'accepted'
                : acceptance.retryable === true
                  ? 'retryable_failed'
                  : 'permanent_failed';
            await tx.deliveries.saveAttempt({
                ...attempt,
                status,
                ...(acceptance.platformMessageId === undefined
                    ? {}
                    : { platformMessageId: acceptance.platformMessageId }),
                ...(acceptance.errorCode === undefined ? {} : { errorCode: acceptance.errorCode }),
                completedAt: this.clock.now(),
            });
            const updated: Delivery = {
                ...target.delivery,
                status,
                ...(acceptance.platformMessageId === undefined
                    ? {}
                    : { externalMessageId: acceptance.platformMessageId }),
                ...(acceptance.errorCode === undefined ? {} : { lastErrorCode: acceptance.errorCode }),
                updatedAt: this.clock.now(),
            };
            await tx.deliveries.save(updated);
            if (status === 'retryable_failed') {
                await tx.outbox.append({
                    id: this.ids.nextOutboxEventId(),
                    eventType: 'im.delivery.retry-scheduled',
                    aggregateId: deliveryId,
                    payload: { deliveryId },
                    status: 'pending',
                    attempts: attempt.attemptNo,
                    availableAt: this.clock.addMinutes(this.clock.now(), 1),
                    createdAt: this.clock.now(),
                });
            }
            return updated;
        });
    }

    /** Retry workers call this after their configured retry budget is exhausted. */
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

export class DefaultReceiptApplication implements ReceiptApplication {
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
    ) {}

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
            await tx.deliveries.saveReceipt({
                id: this.ids.nextDeliveryReceiptId(),
                deliveryId: delivery.id,
                ...(receipt.attemptId === undefined ? {} : { attemptId: receipt.attemptId }),
                stage: receipt.stage,
                dedupeKey: receipt.dedupeKey,
                externalEventId: receipt.externalEventId,
                ...(receipt.detail === undefined ? {} : { detail: receipt.detail }),
                occurredAt: receipt.occurredAt,
                receivedAt: this.clock.now(),
            });
            const status = advanceDeliveryStatus(delivery.status, receipt.stage);
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

export class DefaultActionApplication implements ActionApplication {
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly stream: ActionCommandStreamPort,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
    ) {}

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
                actions: metadata.actions,
                expiresAt: claims.expiresAt,
            };
        });
    }

    public async triggerPrepared(command: TriggerPreparedActionCommand): Promise<ReminderActionCommand> {
        const actionParams = validateReminderActionParams(command.actionType, command.actionParams);
        const prepared = await this.unitOfWork.transaction(async (tx) => {
            const existing = await tx.actions.findById(command.claims.actionId);
            if (existing !== undefined) {
                if (
                    existing.deliveryId !== command.claims.deliveryId ||
                    existing.actionType !== command.actionType ||
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
                !metadata.actions.includes(command.actionType) ||
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

    public resolveActionWindow(deviceId: DeviceId, reminderTriggerId: ReminderTriggerId): Promise<IsoDateTime> {
        return this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findActiveActionWindow(deviceId, reminderTriggerId, this.clock.now());
            if (delivery?.expiresAt === undefined) {
                throw new ImGatewayError('action_expired', 'No active strong-reminder action window was found');
            }
            return delivery.expiresAt;
        });
    }

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

    public find(actionId: ActionId): Promise<ImAction | undefined> {
        return this.unitOfWork.transaction((tx) => tx.actions.findById(actionId));
    }

    public findByOperationId(operationId: OperationId): Promise<ImAction | undefined> {
        return this.unitOfWork.transaction((tx) => tx.actions.findByOperationId(operationId));
    }

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
            const start =
                after === undefined
                    ? 0
                    : Math.max(
                          0,
                          actions.findIndex((action) => action.id === after),
                      );
            const replay = actions.slice(start);
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

export class DefaultActionUiApplication implements ActionUiApplication {
    public constructor(
        private readonly tokens: ActionTokenPort,
        private readonly actions: ActionApplication,
        private readonly clock: Clock,
    ) {}

    public async issue(deliveryId: DeliveryId): Promise<string> {
        return this.tokens.issue(await this.actions.prepareToken(deliveryId));
    }

    public async show(token: string): Promise<ActionUiView> {
        const claims = await this.tokens.verify(token);
        if (claims.expiresAt <= this.clock.now()) {
            throw new ImGatewayError('action_expired', 'Action UI token has expired');
        }
        return this.actions.inspectPrepared(claims);
    }

    public async execute(
        input: Parameters<ActionUiApplication['execute']>[0],
        context?: Parameters<ActionUiApplication['execute']>[1],
    ): Promise<ReminderActionCommand> {
        const claims = await this.tokens.verify(input.token);
        return this.actions.triggerPrepared({
            claims,
            actionType: input.action,
            actionKeyHash: await this.tokens.fingerprint(input.token),
            ...(context?.actualIdentityId === undefined ? {} : { actualIdentityId: context.actualIdentityId }),
            ...(input.params === undefined ? {} : { actionParams: input.params }),
        });
    }
}

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

function toInboundEventType(type: NormalizedImEvent['type']): NormalizedImEvent['type'] {
    return type;
}

function choosePresentationType(
    capabilities: Awaited<ReturnType<ChannelCapabilityResolver['resolve']>>,
    hasActions: boolean,
): PresentationType {
    if (hasActions && capabilities.nativeAction) return 'native_card';
    if (capabilities.presentationTypes.includes('template')) return 'template';
    if (hasActions && capabilities.actionUi) return 'text_with_action_ui';
    if (capabilities.presentationTypes.includes('rich_text')) return 'rich_text';
    return 'text_with_action_ui';
}

function readStrongReminderMetadata(payload: JsonValue):
    | {
          readonly deviceId: DeviceId;
          readonly reminderTriggerId: ReminderTriggerId;
          readonly actions: readonly ReminderActionCommand['action'][];
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
    const actions: ReminderActionCommand['action'][] = [];
    for (const candidate of payload.actions) {
        if (
            typeof candidate !== 'object' ||
            candidate === null ||
            Array.isArray(candidate) ||
            (candidate.type !== 'acknowledge' && candidate.type !== 'snooze')
        ) {
            continue;
        }
        actions.push(candidate.type);
    }
    return {
        deviceId: payload.recipient.deviceId as DeviceId,
        reminderTriggerId: payload.reminderTriggerId as ReminderTriggerId,
        actions,
    };
}

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

function advanceDeliveryStatus(current: DeliveryStatus, receipt: NormalizedDeliveryReceipt['stage']): DeliveryStatus {
    if (current === 'delivered') return current;
    if (receipt === 'delivered') return 'delivered';
    if (current === 'dead_letter' || current === 'permanent_failed') return current;
    return 'permanent_failed';
}
