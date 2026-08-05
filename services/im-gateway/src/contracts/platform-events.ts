import type { ChannelAccountId, DeliveryAttemptId, ExternalIdentityId, InboundEventId, UserId } from './ids.js';
import type { ReminderActionIntent } from './device-gateway.js';
import type { IsoDateTime, JsonValue } from '../shared/types.js';

export type ImPlatform = 'wechat_official' | 'wecom_aibot' | 'feishu' | 'dingtalk';

interface NormalizedImEventBase {
    readonly id: InboundEventId;
    readonly externalEventId: string;
    readonly platform: ImPlatform;
    readonly channelAccountId: ChannelAccountId;
    readonly externalIdentityId?: ExternalIdentityId;
    readonly occurredAt: IsoDateTime;
}

export interface NormalizedDeliveryReceipt {
    readonly externalEventId: string;
    readonly channelAccountId: ChannelAccountId;
    readonly externalMessageId: string;
    readonly attemptId?: DeliveryAttemptId;
    readonly dedupeKey: string;
    readonly stage: 'delivered' | 'failed';
    readonly occurredAt: IsoDateTime;
    readonly platformCode?: string;
    readonly detail?: JsonValue;
}

export interface NormalizedBindingRequest {
    readonly displayCode: string;
    readonly externalUserId: string;
    readonly userId?: UserId;
    readonly displayName?: string;
}

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
