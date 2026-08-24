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
} from '../contracts/ids.js';
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

/** 注册设备的持久化端口。 */
export interface DeviceRepository {
    /** @param device 新设备。 @returns 创建完成。 */
    create(device: ImDevice): Promise<void>;
    /** @param deviceId 设备标识。 @returns 匹配设备。 */
    findById(deviceId: DeviceId): Promise<ImDevice | undefined>;
    /** @param tokenDigest Token 摘要。 @returns 匹配设备。 */
    findByTokenDigest(tokenDigest: Uint8Array): Promise<ImDevice | undefined>;
    /** @param deviceId 设备标识。 @returns 锁定设备。 */
    lockById(deviceId: DeviceId): Promise<ImDevice | undefined>;
    /** @param userId 可选所有者过滤。 @returns 设备列表。 */
    list(userId?: UserId): Promise<readonly ImDevice[]>;
    /** @param device 设备状态。 @returns 保存完成。 */
    save(device: ImDevice): Promise<void>;
}

/** 配对会话的持久化与过期查询端口。 */
export interface PairingSessionRepository {
    /**
     * 取消设备已有的待确认会话，确保重试创建时旧展示码立即失效。
     * @param deviceId 要清理待确认会话的设备。
     * @returns 清理完成后兑现的 Promise。
     */
    cancelPendingByDevice(deviceId: DeviceId): Promise<void>;
    /**
     * 仅当不存在相同展示码散列的待确认会话时创建会话。
     * @param session 待创建的配对会话。
     * @returns true 表示已创建；false 表示展示码与另一待确认会话冲突。
     */
    createPendingIfAbsent(session: PairingSession): Promise<boolean>;
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
     * 锁定指定展示码对应的待确认会话，直到当前工作单元结束。
     * @param hash 展示码散列。
     * @returns 被锁定的待确认会话，不存在时返回 undefined。
     */
    lockPendingByDisplayCodeHash(hash: string): Promise<PairingSession | undefined>;
    /**
     * 按预查询得到的标识和码摘要重新锁定同一 pending 会话。
     * @param id 会话标识。
     * @param hash 展示码摘要。
     * @returns 锁定会话。
     */
    lockPendingByIdAndDisplayCodeHash(id: PairingSessionId, hash: string): Promise<PairingSession | undefined>;
    /**
     * 仅当会话仍为 pending 时原子改为终态，防止覆盖并发确认。
     * @param id 会话标识。
     * @param status 取消或过期终态。
     * @returns true 表示本次完成状态转换。
     */
    transitionPending(id: PairingSessionId, status: 'cancelled' | 'expired'): Promise<boolean>;
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
     * 按渠道账号与外部身份散列幂等创建身份。
     * @param identity 待创建的外部身份。
     * @returns 新建或并发写入时已存在的权威身份。
     */
    createIfAbsent(identity: ExternalIdentity): Promise<ExternalIdentity>;
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
     * 在读取或写入身份/绑定行前取得绑定替换事务级锁。
     * @returns 锁取得后兑现的 Promise。
     */
    acquireReplacementLock(): Promise<void>;
    /**
     * 按用户、设备与外部身份三元组幂等创建有效绑定。
     * @param binding 待创建的 active 绑定，必须带设备标识。
     * @returns 新建或并发写入时已存在的权威绑定。
     */
    createActiveIfAbsent(binding: ImBinding): Promise<ImBinding>;
    /**
     * 原子替换设备或身份的其他 active 绑定；完全相同组合复用原绑定。
     * @param binding 新绑定。
     * @returns 权威 active 绑定。
     */
    replaceActiveBinding(binding: ImBinding): Promise<ImBinding>;
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
     * @returns 权威投递标识与是否由本次调用新建。
     */
    createIfAbsent(delivery: Delivery): Promise<{ id: DeliveryId; created: boolean }>;
    /**
     * 原子领取投递用于派发：pending/retryable_failed 置为 sending，或重领已过期的 sending claim。
     *
     * 领取返回携带内部 claimToken 的投递；claimToken 由实现生成、不可由调用方伪造，
     * 后续 write-back 需凭 token 经 saveIfClaimed 证明所有权（fencing）。
     * @param deliveryId 投递标识。
     * @param now 领取时刻（UTC ISO）。
     * @param leaseSeconds 领取租约时长；sending 且 claimed_at 早于 now-lease 视为已过期可重领。
     * @returns 领取后的投递，未被领取时返回 undefined。
     */
    claimForDispatch(deliveryId: DeliveryId, now: IsoDateTime, leaseSeconds: number): Promise<Delivery | undefined>;
    /**
     * 凭 claimToken 有界写回投递状态：仅当行仍为 sending 且 claim_token 匹配时生效。
     *
     * 这是派发路径唯一的写原语——成功/重试失败/发送前异常都经此写终态并清理 claim 所有权；
     * 未匹配（失去所有权）返回 undefined，调用方必须放弃 attempt/outbox 回写，避免覆盖新 owner。
     * @param delivery 要写入的投递终态（claimedAt/claimToken 缺省即清理）。
     * @param claimToken 本次派发的所有权令牌。
     * @returns 写入后的投递；所有权丢失时返回 undefined。
     */
    saveIfClaimed(delivery: Delivery, claimToken: string): Promise<Delivery | undefined>;
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
     * 预占请求级幂等键：同键首次写入成功，冲突时保留首条记录（first-write-wins）。
     *
     * 并发下 INSERT 会在冲突键的事务提交后返回既有记录，调用方可据此串行化同键提交。
     * @param record 待预占的受理记录。
     * @returns 是否由本次调用新建，以及当前权威记录。
     */
    createIfAbsent(record: IntentSubmissionRecord): Promise<{ created: boolean; record: IntentSubmissionRecord }>;
    /**
     * 回填已预占幂等键的最终受理结果（仅 claim 持有者在其事务内调用）。
     * @param record 含最终 submission 的受理记录。
     * @returns 保存完成后兑现的 Promise。
     */
    finalizeClaim(record: IntentSubmissionRecord): Promise<void>;
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
     * 按设备结果中的操作标识查询已记录动作。
     * @param operationId 设备上报结果的操作标识。
     * @returns 已记录该结果的动作，不存在时返回 undefined。
     */
    findByResultOperationId(operationId: OperationId): Promise<ImAction | undefined>;
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
    /**
     * 幂等创建动作；并发冲突时返回已存在的权威动作。
     * @param action 待创建动作。
     * @returns 权威动作及本次是否创建。
     */
    createIfAbsent(action: ImAction): Promise<{ readonly action: ImAction; readonly created: boolean }>;
}

/** 服务端事务性发件箱的持久化端口。 */
export interface OutboxRepository {
    /**
     * 向事务性发件箱追加事件。
     * @param event 发件箱事件。
     * @returns 追加完成后兑现的 Promise。
     */
    append(event: ImOutboxEvent): Promise<void>;
    /**
     * 原子领取指定类型且已到可用时间的待发布事件，并把 availableAt 推进到租约截止时间。
     * @param eventTypes 本 worker 可处理的事件类型。
     * @param now 当前时间。
     * @param leaseUntil 本次领取的租约截止时间；进程崩溃后事件可再次领取。
     * @param limit 单批最大事件数。
     * @returns 已领取事件，attempts 已递增。
     */
    claimPending(
        eventTypes: readonly string[],
        now: IsoDateTime,
        leaseUntil: IsoDateTime,
        limit: number,
    ): Promise<readonly ImOutboxEvent[]>;
    /**
     * 把已成功消费的事件标记为发布完成。
     * @param eventId 发件箱事件标识。
     * @param publishedAt 完成时间。
     * @returns 更新完成后兑现的 Promise。
     */
    markPublished(eventId: OutboxEventId, publishedAt: IsoDateTime): Promise<void>;
    /**
     * 把无法消费且不应继续重试的事件标记为失败。
     * @param eventId 发件箱事件标识。
     * @returns 更新完成后兑现的 Promise。
     */
    markFailed(eventId: OutboxEventId): Promise<void>;
}

/** 同一事务内可用的全部 IM 聚合仓储。 */
export interface ImUnitOfWorkContext {
    readonly devices: DeviceRepository;
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
