import type { Brand } from '../shared/types.js';

/** 跨模块业务事件标识。 */
export type EventId = Brand<string, 'EventId'>;
/** 串联同一业务链路的关联标识。 */
export type CorrelationId = Brand<string, 'CorrelationId'>;
/** 单次外部请求标识。 */
export type RequestId = Brand<string, 'RequestId'>;
/** 设备动作执行操作标识。 */
export type OperationId = Brand<string, 'OperationId'>;
/** VoiceLife 用户标识。 */
export type UserId = Brand<string, 'UserId'>;
/** VoiceLife 设备标识。 */
export type DeviceId = Brand<string, 'DeviceId'>;

/** 跨进程传递的日程标识；存储适配器可映射内部主键。 */
export type ScheduleId = Brand<string, 'ScheduleId'>;
/** 定时任务定义标识。 */
export type TimerTaskId = Brand<string, 'TimerTaskId'>;
/** 定时任务实例标识。 */
export type TimerInstanceId = Brand<string, 'TimerInstanceId'>;
/** 一次提醒触发窗口的标识。 */
export type ReminderTriggerId = Brand<string, 'ReminderTriggerId'>;

/** IM 渠道账号标识。 */
export type ChannelAccountId = Brand<string, 'ChannelAccountId'>;
/** IM 配对会话标识。 */
export type PairingSessionId = Brand<string, 'PairingSessionId'>;
/** 外部 IM 身份标识。 */
export type ExternalIdentityId = Brand<string, 'ExternalIdentityId'>;
/** VoiceLife 用户与外部身份的绑定标识。 */
export type BindingId = Brand<string, 'BindingId'>;
/** 规范化入站事件记录标识。 */
export type InboundEventId = Brand<string, 'InboundEventId'>;
/** 一次消息投递标识。 */
export type DeliveryId = Brand<string, 'DeliveryId'>;
/** 一次具体投递尝试标识。 */
export type DeliveryAttemptId = Brand<string, 'DeliveryAttemptId'>;
/** 投递回执标识。 */
export type DeliveryReceiptId = Brand<string, 'DeliveryReceiptId'>;
/** 用户对提醒执行动作的标识。 */
export type ActionId = Brand<string, 'ActionId'>;
/** 事务性发件箱事件标识。 */
export type OutboxEventId = Brand<string, 'OutboxEventId'>;

/**
 * 将已经验证的字符串标记为不透明标识类型。
 * @param value 已由调用方验证的字符串。
 * @returns 带有目标标识品牌的原值。
 */
export function unsafeId<T>(value: string): T {
    return value as T;
}
