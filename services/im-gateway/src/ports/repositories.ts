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

/** 渠道账号的持久化端口。 */
export interface ChannelAccountRepository {
    /**
     * 按标识查询渠道账号。
     * @param id 渠道账号标识。
     * @returns 渠道账号，不存在时返回 undefined。
     */
    findById(id: ChannelAccountId): Promise<ChannelAccount | undefined>;
    /**
     * 保存渠道账号当前状态。
     * @param account 渠道账号。
     * @returns 保存完成后兑现的 Promise。
     */
    save(account: ChannelAccount): Promise<void>;
}

/** 配对会话的持久化与过期查询端口。 */
export interface PairingSessionRepository {
    /**
     * 按标识查询配对会话。
     * @param id 配对会话标识。
     * @returns 配对会话，不存在时返回 undefined。
     */
    findById(id: PairingSessionId): Promise<PairingSession | undefined>;
    /**
     * 按展示码散列查询待确认会话。
     * @param hash 展示码散列。
     * @returns 待确认会话，不存在时返回 undefined。
     */
    findPendingByDisplayCodeHash(hash: string): Promise<PairingSession | undefined>;
    /**
     * 查询截止时间已到的待确认会话。
     * @param now 当前时间。
     * @returns 已到期会话列表。
     */
    findExpiredPairingSessions(now: IsoDateTime): Promise<readonly PairingSession[]>;
    /**
     * 保存配对会话当前状态。
     * @param session 配对会话。
     * @returns 保存完成后兑现的 Promise。
     */
    save(session: PairingSession): Promise<void>;
}

/** 受保护外部身份的持久化端口。 */
export interface IdentityRepository {
    /**
     * 按标识查询外部身份。
     * @param id 外部身份标识。
     * @returns 外部身份，不存在时返回 undefined。
     */
    findById(id: ExternalIdentityId): Promise<ExternalIdentity | undefined>;
    /**
     * 按渠道账号和用户标识散列查询外部身份。
     * @param channelAccountId 渠道账号标识。
     * @param externalUserIdHash 外部用户标识散列。
     * @returns 外部身份，不存在时返回 undefined。
     */
    findByChannelAndHash(
        channelAccountId: ChannelAccountId,
        externalUserIdHash: string,
    ): Promise<ExternalIdentity | undefined>;
    /**
     * 保存外部身份当前状态。
     * @param identity 外部身份。
     * @returns 保存完成后兑现的 Promise。
     */
    save(identity: ExternalIdentity): Promise<void>;
}

/** 用户、设备与外部身份绑定关系的持久化端口。 */
export interface BindingRepository {
    /**
     * 按标识查询绑定。
     * @param id 绑定标识。
     * @returns 绑定，不存在时返回 undefined。
     */
    findById(id: BindingId): Promise<ImBinding | undefined>;
    /**
     * 列出用户当前有效的绑定。
     * @param userId 用户标识。
     * @returns 按优先级排序的绑定列表。
     */
    listActiveByUser(userId: UserId): Promise<readonly ImBinding[]>;
    /**
     * 列出设备当前有效的绑定。
     * @param deviceId 设备标识。
     * @returns 有效绑定列表。
     */
    findActiveByDevice(deviceId: DeviceId): Promise<readonly ImBinding[]>;
    /**
     * 查询外部身份当前有效的绑定。
     * @param externalIdentityId 外部身份标识。
     * @returns 有效绑定，不存在时返回 undefined。
     */
    findActiveByIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined>;
    /**
     * 保存绑定当前状态。
     * @param binding 绑定。
     * @returns 保存完成后兑现的 Promise。
     */
    save(binding: ImBinding): Promise<void>;
}

/** 规范化入站事件的幂等持久化端口。 */
export interface InboundEventRepository {
    /**
     * 按标识查询入站事件记录。
     * @param id 入站事件标识。
     * @returns 入站记录，不存在时返回 undefined。
     */
    findById(id: InboundEventId): Promise<InboundEventRecord | undefined>;
    /**
     * 按渠道账号和平台事件标识查询入站记录。
     * @param channelAccountId 渠道账号标识。
     * @param externalEventId 平台事件标识。
     * @returns 入站记录，不存在时返回 undefined。
     */
    findByExternalEvent(
        channelAccountId: ChannelAccountId,
        externalEventId: string,
    ): Promise<InboundEventRecord | undefined>;
    /**
     * 保存入站事件当前状态。
     * @param event 入站事件记录。
     * @returns 保存完成后兑现的 Promise。
     */
    save(event: InboundEventRecord): Promise<void>;
}

/** 投递、发送尝试与平台回执的聚合持久化端口。 */
export interface DeliveryRepository {
    /**
     * 按标识查询投递。
     * @param id 投递标识。
     * @returns 投递，不存在时返回 undefined。
     */
    findById(id: DeliveryId): Promise<Delivery | undefined>;
    /**
     * 按平台消息标识查询投递。
     * @param channelAccountId 渠道账号标识。
     * @param externalMessageId 平台消息标识。
     * @returns 投递，不存在时返回 undefined。
     */
    findByExternalMessage(channelAccountId: ChannelAccountId, externalMessageId: string): Promise<Delivery | undefined>;
    /**
     * 查询设备提醒当前仍有效的动作投递。
     * @param deviceId 设备标识。
     * @param reminderTriggerId 提醒触发窗口标识。
     * @param now 当前时间。
     * @returns 有效投递，不存在时返回 undefined。
     */
    findActiveActionWindow(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<Delivery | undefined>;
    /**
     * 按业务事件、绑定和投递类型查询投递。
     * @param businessEventId 业务事件标识。
     * @param bindingId 绑定标识。
     * @param kind 投递类型。
     * @returns 投递，不存在时返回 undefined。
     */
    findByBusinessKey(
        businessEventId: EventId,
        bindingId: BindingId,
        kind: Delivery['kind'],
    ): Promise<Delivery | undefined>;
    /**
     * 保存投递当前状态。
     * @param delivery 投递。
     * @returns 保存完成后兑现的 Promise。
     */
    save(delivery: Delivery): Promise<void>;
    /**
     * 按业务键幂等创建投递；同键已存在时保留首条并返回其标识。
     * @param delivery 待创建的投递。
     * @returns 数据库中该业务键对应的权威投递标识。
     */
    createIfAbsent(delivery: Delivery): Promise<DeliveryId>;
    /**
     * 原子领取投递用于派发：仅当状态为 pending 或 retryable_failed 时置为 sending。
     * @param deliveryId 投递标识。
     * @returns 领取后的投递，未被领取时返回 undefined。
     */
    claimForDispatch(deliveryId: DeliveryId): Promise<Delivery | undefined>;
    /**
     * 查询指定序号的发送尝试。
     * @param deliveryId 投递标识。
     * @param attemptNo 尝试序号。
     * @returns 发送尝试，不存在时返回 undefined。
     */
    findAttempt(deliveryId: DeliveryId, attemptNo: number): Promise<DeliveryAttempt | undefined>;
    /**
     * 计算投递的下一个尝试序号。
     * @param deliveryId 投递标识。
     * @returns 下一个尝试序号。
     */
    nextAttemptNo(deliveryId: DeliveryId): Promise<number>;
    /**
     * 列出投递的全部发送尝试。
     * @param deliveryId 投递标识。
     * @returns 按尝试序号排列的记录。
     */
    listAttempts(deliveryId: DeliveryId): Promise<readonly DeliveryAttempt[]>;
    /**
     * 保存一次发送尝试。
     * @param attempt 发送尝试。
     * @returns 保存完成后兑现的 Promise。
     */
    saveAttempt(attempt: DeliveryAttempt): Promise<void>;
    /**
     * 按去重键查询投递回执。
     * @param dedupeKey 回执去重键。
     * @returns 回执，不存在时返回 undefined。
     */
    findReceiptByDedupeKey(dedupeKey: string): Promise<DeliveryReceipt | undefined>;
    /**
     * 列出投递的全部平台回执。
     * @param deliveryId 投递标识。
     * @returns 回执列表。
     */
    listReceipts(deliveryId: DeliveryId): Promise<readonly DeliveryReceipt[]>;
    /**
     * 保存平台投递回执。
     * @param receipt 投递回执。
     * @returns 保存完成后兑现的 Promise。
     */
    saveReceipt(receipt: DeliveryReceipt): Promise<void>;
}

/** 通知意图请求级幂等记录的持久化端口。 */
export interface IntentSubmissionRepository {
    /**
     * 按业务事件和投递类型查询受理记录。
     * @param businessEventId 业务事件标识。
     * @param kind 投递类型。
     * @returns 受理记录，不存在时返回 undefined。
     */
    findByBusinessKey(
        businessEventId: EventId,
        kind: IntentSubmissionRecord['kind'],
    ): Promise<IntentSubmissionRecord | undefined>;
    /**
     * 保存请求级幂等受理记录。
     * @param record 受理记录。
     * @returns 保存完成后兑现的 Promise。
     */
    save(record: IntentSubmissionRecord): Promise<void>;
}

/** 提醒动作及其待处理窗口的持久化端口。 */
export interface ActionRepository {
    /**
     * 按标识查询动作。
     * @param id 动作标识。
     * @returns 动作，不存在时返回 undefined。
     */
    findById(id: ActionId): Promise<ImAction | undefined>;
    /**
     * 按设备操作标识查询动作。
     * @param operationId 设备操作标识。
     * @returns 动作，不存在时返回 undefined。
     */
    findByOperationId(operationId: OperationId): Promise<ImAction | undefined>;
    /**
     * 按动作幂等键查询动作。
     * @param actionKeyHash 动作幂等键散列。
     * @returns 动作，不存在时返回 undefined。
     */
    findByActionKeyHash(actionKeyHash: string): Promise<ImAction | undefined>;
    /**
     * 查询设备提醒窗口内仍待处理的动作。
     * @param deviceId 设备标识。
     * @param reminderTriggerId 提醒触发窗口标识。
     * @param now 当前时间。
     * @returns 按创建顺序排列的待处理动作。
     */
    findPendingByDeviceAndTrigger(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<readonly ImAction[]>;
    /**
     * 查询截止时间已到的未完成动作。
     * @param now 当前时间。
     * @returns 已到期动作列表。
     */
    findExpiredActions(now: IsoDateTime): Promise<readonly ImAction[]>;
    /**
     * 保存动作当前状态。
     * @param action 动作。
     * @returns 保存完成后兑现的 Promise。
     */
    save(action: ImAction): Promise<void>;
}

/** 服务端事务性发件箱的持久化端口。 */
export interface OutboxRepository {
    /**
     * 向事务性发件箱追加事件。
     * @param event 发件箱事件。
     * @returns 追加完成后兑现的 Promise。
     */
    append(event: ImOutboxEvent): Promise<void>;
}

/** 同一事务内可用的全部 IM 聚合仓储。 */
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

/** 为跨聚合操作提供原子事务边界。 */
export interface ImUnitOfWork {
    /**
     * 在同一原子事务内执行跨聚合工作。
     * @param work 使用事务仓储上下文的回调。
     * @returns 回调结果；失败时事务必须回滚。
     */
    transaction<T>(work: (context: ImUnitOfWorkContext) => Promise<T>): Promise<T>;
}
