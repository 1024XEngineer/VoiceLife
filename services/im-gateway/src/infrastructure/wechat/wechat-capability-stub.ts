import type { NotificationIntent, ScheduleReceiptIntent } from '../../contracts/device-gateway.js';
import type { NormalizedImEvent } from '../../contracts/platform-events.js';
import type { PlatformCapabilityPort } from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';
import type { JsonValue } from '../../shared/types.js';
import type { ChannelAccount, ChannelCapabilities } from '../../domain/models.js';

export class WechatCapabilityStub implements PlatformCapabilityPort {
    public readonly platform = 'wechat_official' as const;

    public capabilities(_account: ChannelAccount): Promise<ChannelCapabilities> {
        return Promise.resolve({
            proactiveMessage: true,
            nativeAction: false,
            actionUi: true,
            deliveryReceipt: true,
            presentationTypes: ['template', 'text_with_action_ui'],
        });
    }

    public renderScheduleReceipt(intent: ScheduleReceiptIntent): Promise<JsonValue> {
        return Promise.resolve({ type: 'text', text: intent.summary });
    }

    public renderNotification(intent: NotificationIntent): Promise<JsonValue> {
        return Promise.resolve({
            type: 'wechat_template_stub',
            title: intent.content.title,
            reminderTriggerId: intent.reminderTriggerId,
        });
    }

    public normalizeInbound(_rawEvent: unknown): Promise<NormalizedImEvent> {
        return Promise.reject(
            new ImGatewayError('not_implemented', 'WeChat verification and normalization are intentionally empty'),
        );
    }
}
