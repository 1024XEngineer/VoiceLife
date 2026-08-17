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
    ReminderTriggerId,
    RequestId,
    UserId,
} from '../contracts/ids.js';
import type { NotificationIntent, ReminderActionCommand, ScheduleReceiptIntent } from '../contracts/device-gateway.js';
import type { NormalizedImEvent } from '../contracts/platform-events.js';
import type {
    ChannelAccount,
    ChannelCapabilities,
    ConversationRef,
    Delivery,
    ExternalIdentity,
} from '../domain/models.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';

/** 提供可替换的当前时间与时间运算。 */
export interface Clock {
    /** @returns 当前 ISO 日期时间。 */
    now(): IsoDateTime;
    /**
     * 在给定时间上增加分钟数。
     * @param value 起始日期时间。
     * @param minutes 要增加的分钟数。
     * @returns 计算后的日期时间。
     */
    addMinutes(value: IsoDateTime, minutes: number): IsoDateTime;
}

/** 为 Gateway 各聚合生成不透明标识。 */
export interface IdGenerator {
    /** @returns 新的渠道账号标识。 */
    nextChannelAccountId(): ChannelAccountId;
    /** @returns 新的配对会话标识。 */
    nextPairingSessionId(): PairingSessionId;
    /** @returns 新的绑定标识。 */
    nextBindingId(): BindingId;
    /** @returns 新的外部身份标识。 */
    nextExternalIdentityId(): ExternalIdentityId;
    /** @returns 新的投递标识。 */
    nextDeliveryId(): DeliveryId;
    /** @returns 新的投递尝试标识。 */
    nextDeliveryAttemptId(): DeliveryAttemptId;
    /** @returns 新的投递回执标识。 */
    nextDeliveryReceiptId(): DeliveryReceiptId;
    /**
     * 为投递派生稳定的动作标识。
     * @param deliveryId 投递标识。
     * @returns 与投递稳定关联的动作标识。
     */
    actionIdForDelivery(deliveryId: DeliveryId): ActionId;
    /** @returns 新的操作标识。 */
    nextOperationId(): OperationId;
    /** @returns 新的发件箱事件标识。 */
    nextOutboxEventId(): OutboxEventId;
    /** @returns 新的请求标识。 */
    nextRequestId(): RequestId;
}

/** 外部用户标识加密与散列后的安全表示。 */
export interface ProtectedExternalIdentity {
    readonly ciphertext: string;
    readonly hash: string;
}

/** 保护外部用户明文标识的加密端口。 */
export interface ExternalIdentityProtector {
    /**
     * 加密并散列外部用户标识。
     * @param plainExternalUserId 外部平台提供的明文用户标识。
     * @returns 可安全持久化的身份表示。
     */
    protect(plainExternalUserId: string): Promise<ProtectedExternalIdentity>;
}

/** 签发和校验一次性配对展示码的端口。 */
export interface PairingCodePort {
    /** @returns 新签发的展示码及其持久化散列。 */
    issue(): Promise<{ readonly displayCode: string; readonly hash: string }>;
    /**
     * 对用户提交的展示码执行同源散列。
     * @param displayCode 用户提交的展示码。
     * @returns 用于查找配对会话的散列。
     */
    hash(displayCode: string): Promise<string>;
}

/** 解析指定渠道账号当前能力的端口。 */
export interface ChannelCapabilityResolver {
    /**
     * 解析渠道账号当前可用的能力。
     * @param account 渠道账号。
     * @returns 运行时能力集合。
     */
    resolve(account: ChannelAccount): Promise<ChannelCapabilities>;
}

/** 渠道账号最近一次健康检查结果。 */
export interface ChannelHealth {
    readonly accountId: ChannelAccountId;
    readonly status: 'healthy' | 'degraded' | 'unavailable';
    readonly checkedAt: IsoDateTime;
    readonly detail?: string;
}

/** 探测渠道账号可用性的端口。 */
export interface ChannelHealthPort {
    /**
     * 探测渠道账号当前可用性。
     * @param account 渠道账号。
     * @returns 带检查时间的健康状态。
     */
    check(account: ChannelAccount): Promise<ChannelHealth>;
}

/** 将外部身份解析为可投递私聊会话的端口。 */
export interface ConversationResolverPort {
    /**
     * 将外部身份解析为可投递的私聊会话。
     * @param identity 外部身份。
     * @returns 加密保存的会话引用。
     */
    resolveDirect(identity: ExternalIdentity): Promise<ConversationRef>;
}

/** 发送适配器接收的平台无关消息。 */
export interface OutboundImMessage {
    readonly delivery: Delivery;
    readonly conversation: ConversationRef;
    readonly content: JsonValue;
}

/** 平台接受或拒绝发送请求的即时结果。 */
export interface ImSendAcceptance {
    readonly accepted: boolean;
    readonly platformMessageId?: string;
    readonly retryable?: boolean;
    readonly errorCode?: string;
}

/** 向外部 IM 平台发送已渲染消息的端口。 */
export interface ImChannelPort {
    /**
     * 向外部 IM 平台发送已渲染消息。
     * @param message 平台无关的出站消息。
     * @returns 平台即时受理结果。
     */
    send(message: OutboundImMessage): Promise<ImSendAcceptance>;
}

/** 按渠道能力把语义投递渲染为平台载荷的端口。 */
export interface DeliveryRendererPort {
    /**
     * 按账号能力将语义投递渲染为平台载荷。
     * @param delivery 待渲染投递。
     * @param account 目标渠道账号。
     * @param capabilities 渠道运行时能力。
     * @param context 可选动作令牌等渲染上下文。
     * @returns 平台可消费的 JSON 载荷。
     */
    render(
        delivery: Delivery,
        account: ChannelAccount,
        capabilities: ChannelCapabilities,
        context: {
            readonly actionToken?: string;
        },
    ): Promise<JsonValue>;
}

/** 封装单一 IM 平台能力、渲染与入站归一化的端口。 */
export interface PlatformCapabilityPort {
    readonly platform: ChannelAccount['platform'];
    /**
     * 返回指定账号在本平台上的能力。
     * @param account 渠道账号。
     * @returns 平台能力集合。
     */
    capabilities(account: ChannelAccount): Promise<ChannelCapabilities>;
    /**
     * 渲染日程操作回执消息。
     * @param intent 日程回执意图。
     * @returns 平台消息载荷。
     */
    renderScheduleReceipt(intent: ScheduleReceiptIntent): Promise<JsonValue>;
    /**
     * 渲染提醒通知消息。
     * @param intent 通知意图。
     * @returns 平台消息载荷。
     */
    renderNotification(intent: NotificationIntent): Promise<JsonValue>;
    /**
     * 校验并归一化平台原始入站事件。
     * @param rawEvent 平台提供的原始事件。
     * @returns 平台无关的规范化事件。
     */
    normalizeInbound(rawEvent: unknown): Promise<NormalizedImEvent>;
}

/** 订阅设备动作流时的范围、游标与截止时间。 */
export interface ActionStreamSubscription {
    readonly deviceId: DeviceId;
    readonly reminderTriggerId: ReminderTriggerId;
    /** Server-derived deadline; never accepted from the device request. */
    readonly expiresAt: IsoDateTime;
    readonly lastEventId?: ActionId;
    readonly signal?: AbortSignal;
}

/** 关闭已完成动作流所需的服务端持久化作用域。 */
export interface ActionStreamCloseScope {
    readonly deviceId: DeviceId;
    readonly reminderTriggerId: ReminderTriggerId;
    readonly expiresAt: IsoDateTime;
}

/** 发布并订阅设备提醒动作命令的流端口。 */
export interface ActionCommandStreamPort {
    /**
     * 发布一条待设备消费的动作命令。
     * @param command 动作命令。
     * @returns 发布完成后兑现的 Promise。
     */
    publish(command: ReminderActionCommand): Promise<void>;
    /**
     * 订阅指定设备与提醒窗口的动作命令。
     * @param subscription 订阅范围、游标和截止时间。
     * @returns 可异步迭代的动作命令流。
     */
    subscribe(subscription: ActionStreamSubscription): AsyncIterable<ReminderActionCommand>;
    /**
     * 从命令流中关闭指定动作。
     * @param actionId 动作标识。
     * @param scope 持久化 Action 所属的设备、提醒窗口和截止时间。
     * @returns 关闭完成后兑现的 Promise。
     */
    close(actionId: ActionId, scope: ActionStreamCloseScope): Promise<void>;
}

/** 动作令牌中经过服务端签名保护的声明。 */
export interface ActionTokenClaims {
    readonly actionId: ActionId;
    readonly deliveryId: DeliveryId;
    readonly expiresAt: IsoDateTime;
}

/** 签发、验证和指纹化短期动作令牌的端口。 */
export interface ActionTokenPort {
    /**
     * 签发短期动作令牌。
     * @param claims 受保护的动作声明。
     * @returns 不透明令牌。
     */
    issue(claims: ActionTokenClaims): Promise<string>;
    /**
     * 验证动作令牌的签名与结构。
     * @param token 不透明令牌。
     * @returns 可信动作声明。
     */
    verify(token: string): Promise<ActionTokenClaims>;
    /**
     * 生成可用于幂等判断的令牌指纹。
     * @param token 不透明令牌。
     * @returns 不可逆指纹。
     */
    fingerprint(token: string): Promise<string>;
}

/** 设备访问令牌认证得到的调用主体。 */
export interface DevicePrincipal {
    readonly deviceId: DeviceId;
    /** 与设备凭据绑定、不能由请求正文冒充的 VoiceLife 用户。 */
    readonly userId: UserId;
}

/** 将授权信息认证为设备主体的端口。 */
export interface DeviceAuthenticationPort {
    /**
     * 将授权头认证为设备主体。
     * @param authorization 请求授权信息。
     * @returns 经过认证的设备主体。
     */
    authenticate(authorization: string): Promise<DevicePrincipal>;
}
