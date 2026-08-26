import type { ActionId, DeliveryId, DeviceId, OperationId, ReminderTriggerId, UserId } from '../contracts/ids.js';
import {
    DEVICE_CONTRACT_VERSION,
    type NotificationIntent,
    type NotificationActionOption,
    type NotificationSubmission,
    type ReminderActionCommand,
    type ReminderActionResult,
    type ReminderActionStatusReport,
    type ScheduleReceiptIntent,
    type ScheduleQueryResultIntent,
} from '../contracts/device-gateway.js';
import { parseScheduleQueryResultIntent } from '../contracts/device-gateway-parser.js';
import type { NormalizedDeliveryReceipt, NormalizedImEvent } from '../contracts/platform-events.js';
import type {
    ActionStatus,
    ConversationRef,
    Delivery,
    DeliveryStatus,
    ImAction,
    PresentationType,
} from '../domain/models.js';
import type {
    ActionCommandStreamPort,
    ActionTokenClaims,
    ActionTokenPort,
    ChannelCapabilityResolver,
    Clock,
    ConversationResolverPort,
    DeliveryRendererPort,
    IdGenerator,
    ImChannelPort,
    ImSendAcceptance,
} from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';
import { ImGatewayError } from '../shared/errors.js';
import { canonicalizeJson } from '../shared/json.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';
import type {
    ActionApplication,
    ActionUiApplication,
    ActionUiView,
    DeliveryApplication,
    DeliveryDetails,
    DeliveryDispatchApplication,
    InboundEventApplication,
    NotificationApplication,
    PairingApplication,
    PlatformEventApplication,
    ReceiptApplication,
    ScheduleQueryPageApplication,
    TriggerPreparedActionCommand,
} from './api.js';

export { DefaultBindingApplication } from './binding-application.js';
export { DefaultChannelAccountApplication } from './channel-account-application.js';
export { DefaultInboundEventApplication } from './inbound-event-application.js';
export { DefaultPairingApplication } from './pairing-application.js';

const DEFAULT_ACTION_WINDOW_MINUTES = 10;
const MAX_AUTOMATIC_DELIVERY_ATTEMPTS = 5;
const MAX_DELIVERY_RETRY_DELAY_MINUTES = 30;
const SCHEDULE_QUERY_PAGE_TOKEN_MINUTES = 7 * 24 * 60;
const SCHEDULE_QUERY_PAGE_TOKEN_ACTION_ID = 'schedule-query-read';

/** 派发领取租约时长；sending claim 超过该时长未续期即视为崩溃，允许其他 worker 重领。 */
const DISPATCH_CLAIM_LEASE_SECONDS = 60;

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

    /** {@inheritDoc NotificationApplication.submitScheduleQueryResult} */
    public submitScheduleQueryResult(intent: ScheduleQueryResultIntent): Promise<NotificationSubmission> {
        return this.createDeliveries({
            businessEventId: intent.businessEventId,
            correlationId: intent.correlationId,
            ...(intent.userId === undefined ? {} : { userId: intent.userId }),
            deviceId: intent.deviceId,
            kind: 'schedule_query_result',
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
     * @param scheduleQueryPage 日程查询只读页面服务。
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
        private readonly scheduleQueryPage: ScheduleQueryPageApplication,
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
                const now = this.clock.now();
                const failed: Delivery = {
                    ...withoutDeliveryAttemptOutcome(delivery),
                    status: 'permanent_failed',
                    lastErrorCode: 'delivery_target_unavailable',
                    updatedAt: now,
                };
                const written = await tx.deliveries.saveIfClaimed(failed, delivery.claimToken!);
                if (written === undefined) {
                    return {
                        kind: 'terminal' as const,
                        delivery: (await tx.deliveries.findById(deliveryId)) ?? failed,
                    };
                }
                const attemptNo = await tx.deliveries.nextAttemptNo(deliveryId);
                await tx.deliveries.saveAttempt({
                    id: this.ids.nextDeliveryAttemptId(),
                    deliveryId,
                    attemptNo,
                    requestId: this.ids.nextRequestId(),
                    renderedPayload: {},
                    status: 'permanent_failed',
                    errorCode: 'delivery_target_unavailable',
                    startedAt: now,
                    completedAt: now,
                });
                return { kind: 'terminal' as const, delivery: written };
            }
            return { kind: 'target' as const, delivery, identity, account };
        });
        if (target.kind === 'terminal') return target.delivery;
        const claimToken = target.delivery.claimToken;
        if (claimToken === undefined) {
            throw new Error('Claimed delivery is missing a claim token');
        }

        let preSend: { readonly renderedPayload: JsonValue; readonly conversation: ConversationRef };
        try {
            const capabilities = await this.capabilities.resolve(target.account);
            const actionToken =
                target.delivery.expiresAt === undefined ||
                readStrongReminderMetadata(target.delivery.semanticPayload) === undefined
                    ? undefined
                    : await this.actionUi.issue(target.delivery.id);
            const scheduleQueryToken =
                target.delivery.kind === 'schedule_query_result'
                    ? await this.scheduleQueryPage.issue(target.delivery.id)
                    : undefined;
            const renderedPayload = await this.renderer.render(target.delivery, target.account, capabilities, {
                ...(actionToken === undefined ? {} : { actionToken }),
                ...(scheduleQueryToken === undefined ? {} : { scheduleQueryToken }),
            });
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
        const attemptStatus = acceptance.accepted
            ? 'accepted'
            : acceptance.retryable === true
              ? 'retryable_failed'
              : 'permanent_failed';
        const retriesExhausted =
            attemptStatus === 'retryable_failed' && attempt.attemptNo >= MAX_AUTOMATIC_DELIVERY_ATTEMPTS;
        const status = retriesExhausted ? 'permanent_failed' : attemptStatus;
        const terminal: Delivery = {
            ...withoutDeliveryAttemptOutcome(target.delivery),
            status,
            ...(status !== 'accepted' || acceptance.platformMessageId === undefined
                ? {}
                : { externalMessageId: acceptance.platformMessageId }),
            ...(retriesExhausted
                ? { lastErrorCode: 'delivery_retry_exhausted' }
                : acceptance.errorCode === undefined
                  ? {}
                  : { lastErrorCode: acceptance.errorCode }),
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
                status: attemptStatus,
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
                    availableAt: this.clock.addMinutes(completionTime, deliveryRetryDelayMinutes(attempt.attemptNo)),
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
            const attemptNo = await tx.deliveries.nextAttemptNo(deliveryId);
            const retriesExhausted = attemptNo >= MAX_AUTOMATIC_DELIVERY_ATTEMPTS;
            const failed: Delivery = {
                ...withoutDeliveryAttemptOutcome(delivery),
                status: retriesExhausted ? 'permanent_failed' : 'retryable_failed',
                lastErrorCode: retriesExhausted ? 'delivery_retry_exhausted' : 'pre_send_exception',
                updatedAt: now,
            };
            const written = await tx.deliveries.saveIfClaimed(failed, claimToken);
            if (written === undefined) {
                return (await tx.deliveries.findById(deliveryId)) ?? failed;
            }
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
            if (!retriesExhausted) {
                await tx.outbox.append({
                    id: this.ids.nextOutboxEventId(),
                    eventType: 'im.delivery.retry-scheduled',
                    aggregateId: deliveryId,
                    payload: { deliveryId },
                    status: 'pending',
                    attempts: attemptNo,
                    availableAt: this.clock.addMinutes(now, deliveryRetryDelayMinutes(attemptNo)),
                    createdAt: now,
                });
            }
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
            const receiptStatus = advanceDeliveryStatus(delivery.status, receipt);
            const retriesExhausted =
                receiptStatus === 'retryable_failed' && currentAttempt.attemptNo >= MAX_AUTOMATIC_DELIVERY_ATTEMPTS;
            const status = retriesExhausted || receiptStatus === 'permanent_failed' ? 'dead_letter' : receiptStatus;
            if (status !== delivery.status) {
                const now = this.clock.now();
                await tx.deliveries.save({
                    ...delivery,
                    status,
                    ...(status === 'dead_letter'
                        ? {
                              lastErrorCode: retriesExhausted
                                  ? 'delivery_retry_exhausted'
                                  : (receipt.platformCode ?? 'delivery_receipt_failed'),
                          }
                        : {}),
                    updatedAt: now,
                });
                if (status === 'retryable_failed') {
                    await tx.outbox.append({
                        id: this.ids.nextOutboxEventId(),
                        eventType: 'im.delivery.retry-scheduled',
                        aggregateId: delivery.id,
                        payload: { deliveryId: delivery.id },
                        status: 'pending',
                        attempts: currentAttempt.attemptNo,
                        availableAt: this.clock.addMinutes(now, deliveryRetryDelayMinutes(currentAttempt.attemptNo)),
                        createdAt: now,
                    });
                }
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
                !isActionableDelivery(delivery) ||
                delivery.expiresAt !== claims.expiresAt ||
                claims.expiresAt <= this.clock.now()
            ) {
                throw new ImGatewayError('action_expired', 'Action UI token has expired');
            }
            const existing = await tx.actions.findById(claims.actionId);
            if (existing !== undefined) {
                if (existing.deliveryId !== delivery.id || existing.expiresAt !== claims.expiresAt) {
                    throw new ImGatewayError('action_not_found', 'Action token does not match the consumed action');
                }
                return {
                    state: actionUiState(existing.status),
                    action: existing.actionType,
                    ...actionUiParams(existing),
                    ...(existing.result?.nextTriggerAt === undefined
                        ? {}
                        : { nextTriggerAt: existing.result.nextTriggerAt }),
                    expiresAt: existing.expiresAt,
                };
            }
            return {
                state: 'available',
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
            const delivery = await tx.deliveries.findById(command.claims.deliveryId);
            const metadata = delivery === undefined ? undefined : readStrongReminderMetadata(delivery.semanticPayload);
            const binding = delivery === undefined ? undefined : await tx.bindings.findById(delivery.bindingId);
            const identity =
                binding === undefined ? undefined : await tx.identities.findById(binding.externalIdentityId);
            const account =
                delivery === undefined ? undefined : await tx.channelAccounts.findById(delivery.channelAccountId);
            if (
                delivery === undefined ||
                !isActionableDelivery(delivery) ||
                binding === undefined ||
                binding.status !== 'active' ||
                identity === undefined ||
                identity.status !== 'active' ||
                account === undefined ||
                account.status !== 'active' ||
                (command.actualIdentityId !== undefined && command.actualIdentityId !== binding.externalIdentityId) ||
                metadata === undefined ||
                delivery.expiresAt !== command.claims.expiresAt ||
                command.claims.expiresAt <= this.clock.now()
            ) {
                throw new ImGatewayError('action_expired', 'Action token does not match an active delivery');
            }
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

            if (!hasApprovedActionOption(metadata.options, command.actionType, actionParams)) {
                throw new ImGatewayError('action_expired', 'Action token action is not approved by the delivery');
            }

            const now = this.clock.now();
            const deviceFact = await tx.reminderActionFacts.findLatestByDeviceAndTrigger(
                metadata.deviceId,
                metadata.reminderTriggerId,
            );
            const factReport = deviceFact?.report;
            const factIsTerminal = factReport !== undefined && factReport.status !== 'retryable_failed';
            const factMatches = factIsTerminal && factReport!.action === command.actionType;
            const factConflicts = factIsTerminal && factReport!.action !== command.actionType;
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
                status: factConflicts ? 'failed' : factMatches ? toActionStatus(factReport!.status) : 'pending',
                expiresAt: command.claims.expiresAt,
                createdAt: now,
                updatedAt: now,
                ...(factMatches
                    ? {
                          result: toReminderActionResult(factReport!),
                      }
                    : factConflicts
                      ? {
                            result: {
                                ...toReminderActionResult(factReport!),
                                status: 'failed' as const,
                                errorCode: 'superseded_by_device_action',
                            },
                        }
                      : {}),
            };
            const created = await tx.actions.createIfAbsent(action);
            if (!created.created) {
                const existingAction = created.action;
                if (
                    existingAction.deliveryId !== command.claims.deliveryId ||
                    existingAction.actionType !== command.actionType ||
                    !sameActionParams(existingAction.actionParams, actionParams) ||
                    (command.actualIdentityId !== undefined &&
                        command.actualIdentityId !== existingAction.expectedIdentityId)
                ) {
                    throw new ImGatewayError('action_not_found', 'Action token was already used for another action');
                }
                return { command: toCommand(existingAction), shouldDispatch: false };
            }
            return { command: toCommand(created.action), shouldDispatch: !factMatches && !factConflicts };
        });
        if (prepared.shouldDispatch) await this.dispatch(prepared.command);
        return prepared.command;
    }

    /** {@inheritDoc ActionApplication.recordResult} */
    public recordResult(commandId: ActionId, deviceId: DeviceId, result: ReminderActionResult): Promise<ImAction> {
        return this.recordResultAndClose(commandId, deviceId, result);
    }

    /** {@inheritDoc ActionApplication.recordDeviceActionStatus} */
    public async recordDeviceActionStatus(report: ReminderActionStatusReport): Promise<ImAction | undefined> {
        validateDeviceActionStatusReport(report);
        const fingerprint = canonicalizeJson(report as unknown as JsonValue);
        const updatedActions: ImAction[] = [];
        const outcome = await this.unitOfWork.transaction(async (tx) => {
            const existingEvent = await tx.reminderActionFacts.findByEventId(report.eventId);
            if (existingEvent !== undefined) {
                if (existingEvent.fingerprint !== fingerprint) {
                    throw new ImGatewayError(
                        'idempotency_conflict',
                        'Device action event was reused with different data',
                    );
                }
                const existingAction = await tx.actions.findByOperationId(report.operationId);
                if (existingAction !== undefined && existingAction.deviceId === report.deviceId) return existingAction;
            }

            const existingOperation = await tx.reminderActionFacts.findByDeviceAndOperationId(
                report.deviceId,
                report.operationId,
            );
            if (existingOperation !== undefined && existingOperation.eventId !== report.eventId) {
                throw new ImGatewayError(
                    'idempotency_conflict',
                    'Device action operationId was reused with different data',
                );
            }

            const previousLatest = await tx.reminderActionFacts.findLatestByDeviceAndTrigger(
                report.deviceId,
                report.reminderTriggerId,
            );
            if (
                previousLatest !== undefined &&
                previousLatest.eventId !== report.eventId &&
                Date.parse(report.occurredAt) === Date.parse(previousLatest.report.occurredAt)
            ) {
                throw new ImGatewayError(
                    'invalid_transition',
                    'Concurrent device reports for one reminder must have one authoritative event',
                );
            }
            if (
                previousLatest !== undefined &&
                previousLatest.eventId !== report.eventId &&
                previousLatest.report.status === 'succeeded' &&
                isStrictlyLater(report.occurredAt, previousLatest.report.occurredAt)
            ) {
                throw new ImGatewayError(
                    'invalid_transition',
                    'A later device report cannot overwrite an already succeeded reminder action',
                );
            }

            if (existingEvent === undefined) {
                const created = await tx.reminderActionFacts.createIfAbsent({
                    eventId: report.eventId,
                    fingerprint,
                    report,
                    receivedAt: this.clock.now(),
                });
                if (!created.created && created.fact.fingerprint !== fingerprint) {
                    if (
                        created.fact.report.deviceId === report.deviceId &&
                        created.fact.report.reminderTriggerId === report.reminderTriggerId &&
                        Date.parse(created.fact.report.occurredAt) === Date.parse(report.occurredAt)
                    ) {
                        throw new ImGatewayError(
                            'invalid_transition',
                            'Concurrent device reports for one reminder must have one authoritative event',
                        );
                    }
                    throw new ImGatewayError(
                        'idempotency_conflict',
                        'Device action event was reused with different data',
                    );
                }
            }

            const latest = await tx.reminderActionFacts.findLatestByDeviceAndTrigger(
                report.deviceId,
                report.reminderTriggerId,
            );
            if (latest === undefined || latest.report.eventId !== report.eventId) return undefined;

            const pending = await tx.actions.findPendingByDeviceAndTrigger(
                report.deviceId,
                report.reminderTriggerId,
                this.clock.now(),
            );
            let matching: ImAction | undefined;
            for (const action of pending) {
                const sameKind = action.actionType === report.action;
                const result = sameKind
                    ? toReminderActionResult(report)
                    : {
                          ...toReminderActionResult(report),
                          status: 'failed' as const,
                          errorCode: 'superseded_by_device_action',
                      };
                const nextStatus: ActionStatus = sameKind
                    ? report.status === 'retryable_failed'
                        ? 'pending'
                        : report.status
                    : 'failed';
                const updated: ImAction = { ...action, status: nextStatus, result, updatedAt: this.clock.now() };
                await tx.actions.save(updated);
                updatedActions.push(updated);
                if (sameKind) matching = updated;
            }
            return matching;
        });
        for (const action of updatedActions) {
            if (action.status !== 'pending') {
                await this.stream.close(action.id, {
                    deviceId: action.deviceId,
                    reminderTriggerId: action.reminderTriggerId,
                    expiresAt: action.expiresAt,
                });
            }
        }
        return outcome;
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
            await this.stream.close(updated.id, {
                deviceId: updated.deviceId,
                reminderTriggerId: updated.reminderTriggerId,
                expiresAt: updated.expiresAt,
            });
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
        for (const action of expired) {
            await this.stream.close(action.id, {
                deviceId: action.deviceId,
                reminderTriggerId: action.reminderTriggerId,
                expiresAt: action.expiresAt,
            });
        }
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

/** 日程查询结果只读链接的默认实现。 */
export class DefaultScheduleQueryPageApplication implements ScheduleQueryPageApplication {
    /**
     * @param tokens 受保护链接令牌端口。
     * @param unitOfWork 查询已持久化的投递载荷。
     * @param clock 业务时钟。
     */
    public constructor(
        private readonly tokens: ActionTokenPort,
        private readonly unitOfWork: ImUnitOfWork,
        private readonly clock: Clock,
    ) {}

    /** {@inheritDoc ScheduleQueryPageApplication.issue} */
    public async issue(deliveryId: DeliveryId): Promise<string> {
        await this.loadQueryDelivery(deliveryId);
        return this.tokens.issue({
            actionId: scheduleQueryTokenActionId(deliveryId),
            deliveryId,
            expiresAt: this.clock.addMinutes(this.clock.now(), SCHEDULE_QUERY_PAGE_TOKEN_MINUTES),
        });
    }

    /** {@inheritDoc ScheduleQueryPageApplication.show} */
    public async show(token: string): Promise<ScheduleQueryResultIntent> {
        const claims = await this.tokens.verify(token);
        if (claims.expiresAt <= this.clock.now()) {
            throw new ImGatewayError('action_expired', 'Schedule query page token has expired');
        }
        if (claims.actionId !== scheduleQueryTokenActionId(claims.deliveryId)) {
            throw new ImGatewayError('action_not_found', 'Token is not a schedule query page token');
        }
        return this.loadQueryDelivery(claims.deliveryId);
    }

    private async loadQueryDelivery(deliveryId: DeliveryId): Promise<ScheduleQueryResultIntent> {
        return this.unitOfWork.transaction(async (tx) => {
            const delivery = await tx.deliveries.findById(deliveryId);
            if (delivery === undefined || delivery.kind !== 'schedule_query_result') {
                throw new ImGatewayError('action_not_found', 'Schedule query delivery was not found');
            }
            return parseScheduleQueryResultIntent(delivery.semanticPayload);
        });
    }
}

function scheduleQueryTokenActionId(deliveryId: DeliveryId): ActionId {
    return `${SCHEDULE_QUERY_PAGE_TOKEN_ACTION_ID}:${deliveryId}` as ActionId;
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
        if (typeof candidate.label !== 'string' || !isSafeActionLabel(candidate.label)) return undefined;
        const params =
            candidate.params !== undefined &&
            typeof candidate.params === 'object' &&
            candidate.params !== null &&
            !Array.isArray(candidate.params) &&
            typeof candidate.params.minutes === 'number' &&
            Number.isInteger(candidate.params.minutes) &&
            candidate.params.minutes > 0 &&
            candidate.params.minutes <= MAX_SNOOZE_MINUTES
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

function actionUiState(status: ActionStatus): Exclude<ActionUiView['state'], 'available'> {
    return status === 'pending' || status === 'dispatched' ? 'submitted' : status;
}

function actionUiParams(action: ImAction): { readonly params?: { readonly minutes: number } } {
    if (
        action.actionType === 'snooze' &&
        isJsonObject(action.actionParams) &&
        typeof action.actionParams.minutes === 'number' &&
        Number.isInteger(action.actionParams.minutes)
    ) {
        return { params: { minutes: action.actionParams.minutes } };
    }
    return {};
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
        params.minutes <= 0 ||
        params.minutes > MAX_SNOOZE_MINUTES
    ) {
        throw new ImGatewayError('invalid_transition', 'snooze requires a positive integer params.minutes');
    }
    return { minutes: params.minutes };
}

const MAX_SNOOZE_MINUTES = 24 * 60;
const MAX_ACTION_LABEL_LENGTH = 128;

function isSafeActionLabel(value: string): boolean {
    if (value.trim() === '' || Array.from(value).length > MAX_ACTION_LABEL_LENGTH) return false;
    for (const character of value) {
        const codePoint = character.codePointAt(0);
        if (
            codePoint !== undefined &&
            (codePoint <= 0x1f ||
                (codePoint >= 0x7f && codePoint <= 0x9f) ||
                codePoint === 0x2028 ||
                codePoint === 0x2029 ||
                codePoint === 0x061c ||
                codePoint === 0x200e ||
                codePoint === 0x200f ||
                (codePoint >= 0x202a && codePoint <= 0x202e) ||
                (codePoint >= 0x2066 && codePoint <= 0x2069))
        ) {
            return false;
        }
    }
    return true;
}

function isActionableDelivery(delivery: Delivery): boolean {
    return (
        (delivery.status === 'accepted' || delivery.status === 'delivered') &&
        delivery.externalMessageId !== undefined &&
        delivery.externalMessageId.trim() !== ''
    );
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

function deliveryRetryDelayMinutes(attemptNo: number): number {
    return Math.min(MAX_DELIVERY_RETRY_DELAY_MINUTES, 2 ** Math.max(0, attemptNo - 1));
}

function isJsonObject(value: JsonValue | undefined): value is { readonly [key: string]: JsonValue } {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function toReminderActionResult(report: ReminderActionStatusReport): ReminderActionResult {
    return {
        schemaVersion: report.schemaVersion,
        operationId: report.operationId,
        reminderTriggerId: report.reminderTriggerId,
        status: report.status,
        ...(report.nextTriggerAt === undefined ? {} : { nextTriggerAt: report.nextTriggerAt }),
        ...(report.errorCode === undefined ? {} : { errorCode: report.errorCode }),
        ...(report.details === undefined ? {} : { details: report.details }),
        occurredAt: report.occurredAt,
    };
}

function toActionStatus(status: ReminderActionStatusReport['status']): ActionStatus {
    return status === 'retryable_failed' ? 'pending' : status;
}

/** 设备成功事实的动作与下一次触发时间必须在进入持久化前一致。 */
function validateDeviceActionStatusReport(report: ReminderActionStatusReport): void {
    if (
        report.status === 'succeeded' &&
        ((report.action === 'snooze' && report.nextTriggerAt === undefined) ||
            (report.action === 'acknowledge' && report.nextTriggerAt !== undefined))
    ) {
        throw new ImGatewayError('invalid_transition', 'Device action nextTriggerAt does not match action');
    }
}

/** ISO 8601 已由契约层校验，此处只比较两个已规范化业务时间。 */
function isStrictlyLater(left: IsoDateTime, right: IsoDateTime): boolean {
    return Date.parse(left) > Date.parse(right);
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
