import type {
    ActionId,
    BindingId,
    ChannelAccountId,
    DeliveryId,
    DeviceId,
    ExternalIdentityId,
    OperationId,
    PairingSessionId,
    UserId,
} from '../contracts/ids.js';
import type {
    ActionIntent,
    NotificationIntent,
    NotificationSubmission,
    ReminderActionCommand,
    ReminderActionKind,
    ReminderActionResult,
    ScheduleReceiptIntent,
} from '../contracts/device-gateway.js';
import type { ImPlatform, NormalizedDeliveryReceipt, NormalizedImEvent } from '../contracts/platform-events.js';
import type {
    ChannelAccount,
    Delivery,
    DeliveryAttempt,
    DeliveryReceipt,
    ImAction,
    ImBinding,
    PairingSession,
} from '../domain/models.js';
import type { ActionTokenClaims, ChannelHealth } from '../ports/external.js';
import type { JsonValue } from '../shared/types.js';

export interface RegisterChannelAccountCommand {
    readonly platform: ImPlatform;
    readonly tenantExternalId: string;
    readonly koishiBotId: string;
    readonly credentialRef: string;
    readonly connectionMode: ChannelAccount['connectionMode'];
    readonly capabilityConfig?: JsonValue;
}

export interface ChannelAccountApplication {
    register(command: RegisterChannelAccountCommand): Promise<ChannelAccount>;
    disable(channelAccountId: ChannelAccountId): Promise<void>;
    find(channelAccountId: ChannelAccountId): Promise<ChannelAccount | undefined>;
    health(channelAccountId: ChannelAccountId): Promise<ChannelHealth>;
}

export interface CreatePairingSessionCommand {
    readonly userId?: UserId;
    readonly deviceId: DeviceId;
    readonly allowedPlatforms?: readonly ImPlatform[];
    readonly expiresInMinutes?: number;
}

export interface CreatedPairingSession {
    readonly session: PairingSession;
    readonly displayCode: string;
}

export interface ConfirmPairingCommand {
    readonly displayCode: string;
    readonly channelAccountId: ChannelAccountId;
    readonly externalUserId: string;
    readonly userId?: UserId;
    readonly displayName?: string;
}

export interface PairingApplication {
    create(command: CreatePairingSessionCommand): Promise<CreatedPairingSession>;
    find(pairingSessionId: PairingSessionId): Promise<PairingSession | undefined>;
    confirm(command: ConfirmPairingCommand): Promise<ImBinding>;
    cancel(pairingSessionId: PairingSessionId): Promise<void>;
    expireDue(): Promise<number>;
}

export interface BindingApplication {
    list(userId: UserId): Promise<readonly ImBinding[]>;
    unbind(bindingId: BindingId): Promise<void>;
    revoke(bindingId: BindingId): Promise<void>;
    findActiveByExternalIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined>;
}

export interface NotificationApplication {
    submitScheduleReceipt(intent: ScheduleReceiptIntent): Promise<NotificationSubmission>;
    submitNotification(intent: NotificationIntent): Promise<NotificationSubmission>;
}

export interface DeliveryDetails {
    readonly delivery: Delivery;
    readonly attempts: readonly DeliveryAttempt[];
    readonly receipts: readonly DeliveryReceipt[];
}

export interface DeliveryApplication {
    find(deliveryId: DeliveryId): Promise<DeliveryDetails | undefined>;
    retryDeadLetter(deliveryId: DeliveryId): Promise<Delivery>;
}

export interface DeliveryDispatchApplication {
    dispatch(deliveryId: DeliveryId): Promise<Delivery>;
    markDeadLetter(deliveryId: DeliveryId): Promise<Delivery>;
}

export interface ReceiptApplication {
    record(receipt: NormalizedDeliveryReceipt): Promise<void>;
}

export interface InboundEventApplication {
    recordIfNew(event: NormalizedImEvent): Promise<'accepted' | 'duplicate'>;
    markProcessing(eventId: NormalizedImEvent['id']): Promise<void>;
    markProcessed(eventId: NormalizedImEvent['id']): Promise<void>;
    markFailed(eventId: NormalizedImEvent['id']): Promise<void>;
}

/** Same-process entry used by Koishi/Capability adapters after normalization. */
export interface PlatformEventApplication {
    postEvent(event: NormalizedImEvent): Promise<void | ReminderActionCommand>;
}

export interface TriggerPreparedActionCommand {
    readonly claims: ActionTokenClaims;
    readonly actionType: ReminderActionKind;
    readonly actionParams?: JsonValue;
    readonly actionKeyHash: string;
    readonly actualIdentityId?: ExternalIdentityId;
}

export interface ActionUiView {
    readonly actionId: ActionId;
    readonly actions: readonly ReminderActionKind[];
    readonly expiresAt: ImAction['expiresAt'];
}

export interface ActionApplication {
    prepareToken(deliveryId: DeliveryId): Promise<ActionTokenClaims>;
    inspectPrepared(claims: ActionTokenClaims): Promise<ActionUiView>;
    triggerPrepared(command: TriggerPreparedActionCommand): Promise<ReminderActionCommand>;
    markProcessing(
        actionId: ActionId,
        deviceId: DeviceId,
        reminderTriggerId: ReminderActionCommand['reminderTriggerId'],
    ): Promise<void>;
    recordResult(commandId: ActionId, deviceId: DeviceId, result: ReminderActionResult): Promise<ImAction>;
    expireDue(): Promise<number>;
    resolveActionWindow(
        deviceId: DeviceId,
        reminderTriggerId: ReminderActionCommand['reminderTriggerId'],
    ): Promise<ImAction['expiresAt']>;
    find(actionId: ActionId): Promise<ImAction | undefined>;
    findByOperationId(operationId: OperationId): Promise<ImAction | undefined>;
    replayPending(
        deviceId: DeviceId,
        reminderTriggerId: ReminderActionCommand['reminderTriggerId'],
        after?: ActionId,
    ): Promise<readonly ReminderActionCommand[]>;
}

export interface ActionUiApplication {
    issue(deliveryId: DeliveryId): Promise<string>;
    show(token: string): Promise<ActionUiView>;
    execute(
        input: Pick<ActionIntent, 'token' | 'params'> & {
            readonly action: ReminderActionKind;
        },
        context?: {
            readonly actualIdentityId?: ExternalIdentityId;
        },
    ): Promise<ReminderActionCommand>;
}

export interface ImGatewayApplication {
    readonly channels: ChannelAccountApplication;
    readonly pairing: PairingApplication;
    readonly bindings: BindingApplication;
    readonly inboundEvents: InboundEventApplication;
    readonly platformEvents: PlatformEventApplication;
    readonly notifications: NotificationApplication;
    readonly deliveries: DeliveryApplication;
    readonly deliveryDispatch: DeliveryDispatchApplication;
    readonly receipts: ReceiptApplication;
    readonly actions: ActionApplication;
    readonly actionUi: ActionUiApplication;
}
