import type { ChannelAccountId, DeliveryAttemptId, ExternalIdentityId, InboundEventId, UserId } from './ids.js';
import type { ReminderActionIntent } from './device-gateway.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';

/** Gateway 支持的 IM 平台。 */
export type ImPlatform = 'wechat_official' | 'feishu' | 'dingtalk';

interface NormalizedImEventBase {
    readonly id: InboundEventId;
    readonly externalEventId: string;
    readonly platform: ImPlatform;
    readonly channelAccountId: ChannelAccountId;
    readonly externalIdentityId?: ExternalIdentityId;
    readonly occurredAt: IsoDateTime;
}

/** 平台投递状态回调归一化后的回执。 */
export interface NormalizedDeliveryReceipt {
    readonly externalEventId: string;
    readonly channelAccountId: ChannelAccountId;
    readonly externalMessageId: string;
    readonly attemptId?: DeliveryAttemptId;
    readonly dedupeKey: string;
    readonly stage: 'delivered' | 'failed';
    readonly retryable?: boolean;
    readonly occurredAt: IsoDateTime;
    readonly platformCode?: string;
    readonly detail?: JsonValue;
}

/** 外部用户发起的账号绑定请求。 */
export interface NormalizedBindingRequest {
    readonly displayCode: string;
    readonly externalUserId: string;
    readonly userId?: UserId;
    readonly displayName?: string;
}

/** 平台入站事件归一化后的判别联合。 */
export type NormalizedImEvent =
    | (NormalizedImEventBase & {
          readonly type: 'message.received';
          readonly payload: JsonValue;
      })
    | (NormalizedImEventBase & {
          readonly type: 'binding.requested';
          readonly payload: NormalizedBindingRequest;
      })
    | (NormalizedImEventBase & {
          readonly type: 'delivery.updated';
          readonly payload: NormalizedDeliveryReceipt;
      })
    | (NormalizedImEventBase & {
          readonly type: 'action.triggered';
          readonly payload: ReminderActionIntent;
      });
