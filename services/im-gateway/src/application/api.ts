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
    CreatePairingSessionRequest,
    NotificationIntent,
    NotificationSubmission,
    ReminderActionCommand,
    ReminderActionKind,
    ReminderActionResult,
    VoiceReminderActionStatus,
    ScheduleReceiptIntent,
    ScheduleQueryResultIntent,
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

/** 注册一个租户 IM 渠道账号所需的参数。 */
export interface RegisterChannelAccountCommand {
    readonly platform: ImPlatform;
    readonly tenantExternalId: string;
    readonly koishiBotId: string;
    readonly credentialRef: string;
    readonly connectionMode: ChannelAccount['connectionMode'];
    readonly capabilityConfig?: JsonValue;
}

/** 管理渠道账号生命周期及健康状态的应用服务。 */
export interface ChannelAccountApplication {
    /**
     * 注册并持久化一个渠道账号。
     * @param command 渠道账号配置。
     * @returns 新注册的渠道账号。
     */
    register(command: RegisterChannelAccountCommand): Promise<ChannelAccount>;
    /**
     * 停用指定渠道账号。
     * @param channelAccountId 渠道账号标识。
     * @returns 停用操作完成后兑现的 Promise。
     */
    disable(channelAccountId: ChannelAccountId): Promise<void>;
    /**
     * 按标识查询渠道账号。
     * @param channelAccountId 渠道账号标识。
     * @returns 渠道账号，不存在时返回 undefined。
     */
    find(channelAccountId: ChannelAccountId): Promise<ChannelAccount | undefined>;
    /**
     * 查询渠道账号的实时健康状态。
     * @param channelAccountId 渠道账号标识。
     * @returns 最近一次健康检查结果。
     */
    health(channelAccountId: ChannelAccountId): Promise<ChannelHealth>;
}

/** 创建短期配对会话所需的参数。 */
export type CreatePairingSessionCommand = CreatePairingSessionRequest;

/** 新建配对会话及其一次性展示码。 */
export interface CreatedPairingSession {
    readonly session: PairingSession;
    readonly displayCode: string;
}

/** 使用展示码确认外部身份绑定所需的参数。 */
export interface ConfirmPairingCommand {
    readonly displayCode: string;
    readonly channelAccountId: ChannelAccountId;
    readonly externalUserId: string;
    readonly userId?: UserId;
    readonly displayName?: string;
}

/** 管理配对会话创建、确认和过期的应用服务。 */
export interface PairingApplication {
    /**
     * 创建带有一次性展示码的配对会话。
     * @param command 配对范围与有效期配置。
     * @returns 新会话及仅供展示的明文配对码。
     */
    create(command: CreatePairingSessionCommand): Promise<CreatedPairingSession>;
    /**
     * 按标识查询配对会话。
     * @param pairingSessionId 配对会话标识。
     * @returns 配对会话，不存在时返回 undefined。
     */
    find(pairingSessionId: PairingSessionId): Promise<PairingSession | undefined>;
    /**
     * 校验展示码并建立外部身份绑定。
     * @param command 配对确认参数。
     * @returns 新建或恢复的有效绑定。
     */
    confirm(command: ConfirmPairingCommand): Promise<ImBinding>;
    /**
     * 取消尚未完成的配对会话。
     * @param pairingSessionId 配对会话标识。
     * @returns 取消操作完成后兑现的 Promise。
     */
    cancel(pairingSessionId: PairingSessionId): Promise<void>;
    /**
     * 将所有超过截止时间的待确认会话标记为过期。
     * @returns 本次过期的会话数量。
     */
    expireDue(): Promise<number>;
}

/** 查询和解除外部身份绑定的应用服务。 */
export interface BindingApplication {
    /**
     * 列出用户当前有效的外部身份绑定。
     * @param userId VoiceLife 用户标识。
     * @returns 按优先级排序的有效绑定。
     */
    list(userId: UserId): Promise<readonly ImBinding[]>;
    /**
     * 由用户主动解除一条绑定。
     * @param bindingId 绑定标识。
     * @returns 解绑完成后兑现的 Promise。
     */
    unbind(bindingId: BindingId): Promise<void>;
    /**
     * 由系统撤销一条绑定。
     * @param bindingId 绑定标识。
     * @returns 撤销完成后兑现的 Promise。
     */
    revoke(bindingId: BindingId): Promise<void>;
    /**
     * 查询外部身份当前对应的有效绑定。
     * @param externalIdentityId 外部身份标识。
     * @returns 有效绑定，不存在时返回 undefined。
     */
    findActiveByExternalIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined>;
}

/** 受理日程回执与提醒通知意图的应用服务。 */
export interface NotificationApplication {
    /**
     * 幂等受理设备上报的日程操作回执。
     * @param intent 已校验的日程回执意图。
     * @returns 投递受理结果。
     */
    submitScheduleReceipt(intent: ScheduleReceiptIntent): Promise<NotificationSubmission>;
    /**
     * 幂等受理设备上报的完整日程查询结果。
     * @param intent 已校验的完整查询结果意图。
     * @returns 投递受理结果。
     */
    submitScheduleQueryResult(intent: ScheduleQueryResultIntent): Promise<NotificationSubmission>;
    /**
     * 幂等受理设备发起的提醒通知。
     * @param intent 已校验的通知意图。
     * @returns 投递受理结果。
     */
    submitNotification(intent: NotificationIntent): Promise<NotificationSubmission>;
}

/** 一次投递及其尝试和回执的聚合视图。 */
export interface DeliveryDetails {
    readonly delivery: Delivery;
    readonly attempts: readonly DeliveryAttempt[];
    readonly receipts: readonly DeliveryReceipt[];
}

/** 查询投递详情并恢复死信或永久失败投递的应用服务。 */
export interface DeliveryApplication {
    /**
     * 查询一次投递及其全部尝试和回执。
     * @param deliveryId 投递标识。
     * @returns 投递聚合详情，不存在时返回 undefined。
     */
    find(deliveryId: DeliveryId): Promise<DeliveryDetails | undefined>;
    /**
     * 将死信或永久失败投递恢复为可再次发送状态。
     * @param deliveryId 投递标识。
     * @returns 更新后的投递。
     */
    retryDeadLetter(deliveryId: DeliveryId): Promise<Delivery>;
}

/** 执行消息投递及死信转换的应用服务。 */
export interface DeliveryDispatchApplication {
    /**
     * 渲染并发送指定投递，同时记录发送尝试。
     * @param deliveryId 投递标识。
     * @returns 发送推进后的投递。
     */
    dispatch(deliveryId: DeliveryId): Promise<Delivery>;
    /**
     * 将指定投递标记为死信。
     * @param deliveryId 投递标识。
     * @returns 更新后的投递。
     */
    markDeadLetter(deliveryId: DeliveryId): Promise<Delivery>;
}

/** 归并平台投递回执的应用服务。 */
export interface ReceiptApplication {
    /**
     * 幂等记录并归并平台投递回执。
     * @param receipt 规范化的平台回执。
     * @returns 回执处理完成后兑现的 Promise。
     */
    record(receipt: NormalizedDeliveryReceipt): Promise<void>;
}

/** 持久化并推进规范化入站事件状态的应用服务。 */
export interface InboundEventApplication {
    /**
     * 仅在事件首次出现时创建入站记录；失败记录可在平台重放时重新进入处理流程。
     * @param event 规范化入站事件。
     * @returns accepted 表示首次事件或失败重放，duplicate 表示已存在且无需处理。
     */
    recordIfNew(event: NormalizedImEvent): Promise<'accepted' | 'duplicate'>;
    /**
     * 将入站事件推进为处理中。
     * @param eventId 入站事件标识。
     * @returns 状态更新完成后兑现的 Promise。
     */
    markProcessing(eventId: NormalizedImEvent['id']): Promise<void>;
    /**
     * 将入站事件推进为处理成功。
     * @param eventId 入站事件标识。
     * @returns 状态更新完成后兑现的 Promise。
     */
    markProcessed(eventId: NormalizedImEvent['id']): Promise<void>;
    /**
     * 将入站事件推进为处理失败。
     * @param eventId 入站事件标识。
     * @returns 状态更新完成后兑现的 Promise。
     */
    markFailed(eventId: NormalizedImEvent['id']): Promise<void>;
}

/** Koishi 与 Capability 适配器完成归一化后的同进程入口。 */
export interface PlatformEventApplication {
    /**
     * 路由一个已经完成平台归一化的入站事件。
     * @param event 规范化入站事件。
     * @returns 动作事件对应的设备命令；其他事件不返回值。
     */
    postEvent(event: NormalizedImEvent): Promise<void | ReminderActionCommand>;
}

/** 触发一个已经校验并准备好的用户动作所需的参数。 */
export interface TriggerPreparedActionCommand {
    readonly claims: ActionTokenClaims;
    readonly actionType: ReminderActionKind;
    readonly actionParams?: JsonValue;
    readonly actionKeyHash: string;
    readonly actualIdentityId?: ExternalIdentityId;
}

/** 尚未提交动作时，页面呈现给用户的服务端批准选项。 */
export interface AvailableActionUiView {
    readonly state: 'available';
    readonly actionId: ActionId;
    readonly actions: readonly ReminderActionKind[];
    /** 页面可以呈现的服务端批准动作、文案与固定参数。 */
    readonly options: readonly ActionUiOption[];
    readonly expiresAt: ImAction['expiresAt'];
}

/** 动作经任一入口消费后，页面可以安全公开的统一生命周期状态。 */
export interface ConsumedActionUiView {
    readonly state: 'submitted' | 'processing' | 'succeeded' | 'failed' | 'expired';
    readonly action: ReminderActionKind;
    readonly params?: { readonly minutes: number };
    readonly expiresAt: ImAction['expiresAt'];
}

/** 动作页面呈现给用户的安全只读视图。 */
export type ActionUiView = AvailableActionUiView | ConsumedActionUiView;

/** 动作页面可以提交的单个服务端批准选项。 */
export interface ActionUiOption {
    readonly action: ReminderActionKind;
    readonly label: string;
    readonly params?: { readonly minutes: number };
}

/** 管理提醒动作准备、触发、回放与结果归并的应用服务。 */
export interface ActionApplication {
    /**
     * 为投递准备服务端可信的动作令牌声明。
     * @param deliveryId 投递标识。
     * @returns 可签发为短期令牌的声明。
     */
    prepareToken(deliveryId: DeliveryId): Promise<ActionTokenClaims>;
    /**
     * 在不执行动作的情况下读取已准备动作的安全视图。
     * @param claims 已验证的动作令牌声明。
     * @returns 可展示给用户的动作视图。
     */
    inspectPrepared(claims: ActionTokenClaims): Promise<ActionUiView>;
    /**
     * 幂等触发一个已经验证并准备好的动作。
     * @param command 动作声明、类型和身份上下文。
     * @returns 下发给目标设备的动作命令。
     */
    triggerPrepared(command: TriggerPreparedActionCommand): Promise<ReminderActionCommand>;
    /**
     * 确认设备已经开始处理动作命令。
     * @param actionId 动作标识。
     * @param deviceId 执行动作的设备标识。
     * @param reminderTriggerId 提醒触发窗口标识。
     * @returns 状态更新完成后兑现的 Promise。
     */
    markProcessing(
        actionId: ActionId,
        deviceId: DeviceId,
        reminderTriggerId: ReminderActionCommand['reminderTriggerId'],
    ): Promise<void>;
    /**
     * 记录设备返回的动作执行结果。
     * @param commandId 动作命令标识。
     * @param deviceId 回传结果的设备标识。
     * @param result 已校验的执行结果。
     * @returns 归并结果后的动作记录。
     */
    recordResult(commandId: ActionId, deviceId: DeviceId, result: ReminderActionResult): Promise<ImAction>;
    /**
     * 归并设备语音直接消费的动作状态，并关闭同一提醒的其他待处理动作。
     * @param status 已校验的设备语音动作状态。
     * @returns 归并结果后的动作记录。
     */
    recordVoiceStatus(status: VoiceReminderActionStatus): Promise<readonly ImAction[]>;
    /**
     * 关闭所有超过截止时间的未完成动作。
     * @returns 本次过期的动作数量。
     */
    expireDue(): Promise<number>;
    /**
     * 查询设备上指定提醒仍然有效的动作窗口截止时间。
     * @param deviceId 目标设备标识。
     * @param reminderTriggerId 提醒触发窗口标识。
     * @returns 服务端确定的动作截止时间。
     */
    resolveActionWindow(
        deviceId: DeviceId,
        reminderTriggerId: ReminderActionCommand['reminderTriggerId'],
    ): Promise<ImAction['expiresAt']>;
    /**
     * 按标识查询动作记录。
     * @param actionId 动作标识。
     * @returns 动作记录，不存在时返回 undefined。
     */
    find(actionId: ActionId): Promise<ImAction | undefined>;
    /**
     * 按设备操作标识查询动作记录。
     * @param operationId 设备操作标识。
     * @returns 动作记录，不存在时返回 undefined。
     */
    findByOperationId(operationId: OperationId): Promise<ImAction | undefined>;
    /**
     * 回放窗口内所有仍未确认、未过期的动作命令；传输游标不代表业务确认。
     * @param deviceId 目标设备标识。
     * @param reminderTriggerId 提醒触发窗口标识。
     * @param after 设备已收到的最新动作标识，仅作为续接上下文，不能排除未确认动作。
     * @returns 有序的待处理动作命令。
     */
    replayPending(
        deviceId: DeviceId,
        reminderTriggerId: ReminderActionCommand['reminderTriggerId'],
        after?: ActionId,
    ): Promise<readonly ReminderActionCommand[]>;
}

/** 为 H5 或小程序动作页面提供令牌入口的应用服务。 */
export interface ActionUiApplication {
    /**
     * 为指定投递签发短期动作令牌。
     * @param deliveryId 投递标识。
     * @returns 可交给动作页面的令牌。
     */
    issue(deliveryId: DeliveryId): Promise<string>;
    /**
     * 校验令牌并构造动作页面视图。
     * @param token 不透明动作令牌。
     * @returns 可展示的动作视图。
     */
    show(token: string): Promise<ActionUiView>;
    /**
     * 校验并执行动作页面提交的操作。
     * @param input 令牌、动作类型与动作参数。
     * @param context 可选的实际外部身份上下文。
     * @returns 下发给设备的动作命令。
     */
    execute(
        input: Pick<ActionIntent, 'token' | 'params'> & {
            readonly action: ReminderActionKind;
        },
        context?: {
            readonly actualIdentityId?: ExternalIdentityId;
        },
    ): Promise<ReminderActionCommand>;
}

/** 为日程查询结果提供受保护的只读页面入口。 */
export interface ScheduleQueryPageApplication {
    /**
     * 为已经持久化的日程查询投递签发短期只读链接。
     * @param deliveryId 日程查询结果投递标识。
     * @returns 可用于 H5 页面路由的不透明令牌。
     */
    issue(deliveryId: DeliveryId): Promise<string>;
    /**
     * 校验只读令牌并返回可安全呈现的完整查询结果。
     * @param token 不透明查询结果令牌。
     * @returns 已校验的查询结果。
     */
    show(token: string): Promise<ScheduleQueryResultIntent>;
}

/** IM Gateway 全部应用服务的统一访问入口。 */
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
    readonly scheduleQueryPage: ScheduleQueryPageApplication;
}
