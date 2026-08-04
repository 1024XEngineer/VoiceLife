import { DEVICE_CONTRACT_VERSION } from '../contracts/device-gateway.js';
import type {
    ActionId,
    CorrelationId,
    DeviceId,
    EventId,
    InboundEventId,
    ReminderTriggerId,
    ScheduleId,
    TimerInstanceId,
    TimerTaskId,
    UserId,
} from '../contracts/ids.js';
import { unsafeId } from '../contracts/ids.js';
import type { IsoDateTime } from '../shared/types.js';
import { ImGatewayError } from '../shared/errors.js';
import { FixedClock } from '../infrastructure/mock-support.js';
import { createMockImGateway } from './create-im-gateway.js';

/** Minimal executable seam for future architecture tests. */
export async function runMockNotificationScenario(): Promise<void> {
    const deviceId = unsafeId<DeviceId>('device-demo');
    const userId = unsafeId<UserId>('user-demo');
    const clock = new FixedClock();
    const gateway = createMockImGateway(deviceId, clock);
    const channel = await gateway.application.channels.register({
        platform: 'wechat_official',
        tenantExternalId: 'wechat-app-demo',
        koishiBotId: 'wechat-main',
        credentialRef: 'secret://wechat-main',
        connectionMode: 'webhook',
    });
    const pairing = await gateway.deviceApi.postPairingSession({
        authorization: 'Bearer mock-device-token',
        body: { userId, deviceId },
    });
    const pairingStatus = await gateway.deviceApi.getPairingSession({
        authorization: 'Bearer mock-device-token',
        pairingSessionId: pairing.session.id,
    });
    if (pairingStatus?.status !== 'pending') {
        throw new Error('PairingSession device HTTPS mapping is incorrect');
    }
    const binding = await gateway.application.pairing.confirm({
        displayCode: pairing.displayCode,
        channelAccountId: channel.id,
        externalUserId: 'mock-open-id',
    });

    const conflictingPairing = await gateway.deviceApi.postPairingSession({
        authorization: 'Bearer mock-device-token',
        body: { userId, deviceId },
    });
    let mismatchedUserRejected = false;
    try {
        await gateway.application.pairing.confirm({
            displayCode: conflictingPairing.displayCode,
            channelAccountId: channel.id,
            externalUserId: 'another-open-id',
            userId: unsafeId<UserId>('user-other'),
        });
    } catch (error) {
        mismatchedUserRejected = error instanceof ImGatewayError && error.code === 'invalid_transition';
    }
    if (!mismatchedUserRejected) {
        throw new Error('Pairing confirmation accepted a mismatched session user');
    }
    await gateway.application.pairing.cancel(conflictingPairing.session.id);

    const scheduleReceipt = await gateway.deviceApi.postScheduleReceipt({
        authorization: 'Bearer mock-device-token',
        idempotencyKey: 'event-schedule-receipt',
        body: {
            schemaVersion: DEVICE_CONTRACT_VERSION,
            eventId: unsafeId<EventId>('event-schedule-receipt'),
            correlationId: unsafeId<CorrelationId>('correlation-schedule-receipt'),
            userId,
            deviceId,
            operationType: 'created',
            scheduleId: unsafeId<ScheduleId>('schedule-100'),
            result: 'succeeded',
            summary: '日程已创建',
            occurredAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
        },
    });
    if (scheduleReceipt.deliveries.length !== 1) {
        throw new Error('ScheduleReceiptIntent HTTPS mapping did not create Delivery');
    }

    const strongIntent = {
        schemaVersion: DEVICE_CONTRACT_VERSION,
        businessEventId: unsafeId<EventId>('event-strong'),
        correlationId: unsafeId<CorrelationId>('correlation-demo'),
        kind: 'reminder_due',
        recipient: { userId, deviceId },
        scheduleId: unsafeId<ScheduleId>('schedule-1'),
        taskId: unsafeId<TimerTaskId>('task-demo'),
        instanceId: unsafeId<TimerInstanceId>('instance-demo'),
        reminderTriggerId: unsafeId<ReminderTriggerId>('trigger-demo'),
        reminderType: 'strong',
        content: { title: 'Mock reminder' },
        plannedAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
        triggerAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
        actions: [
            { kind: 'command', type: 'acknowledge', label: '知道了' },
            {
                kind: 'command',
                type: 'snooze',
                label: '推迟 5 分钟',
                params: { minutes: 5 },
            },
        ],
        occurredAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
    } as const;
    const strong = await gateway.deviceApi.postNotification({
        authorization: 'Bearer mock-device-token',
        idempotencyKey: 'event-strong',
        body: strongIntent,
    });
    if (strong.deliveries.length !== 1 || strong.actionStream === undefined) {
        throw new Error('Strong reminder did not create Delivery and action window');
    }
    const strongDelivery = strong.deliveries[0];
    if (strongDelivery === undefined) throw new Error('Strong Delivery is missing');
    const replayedStrong = await gateway.deviceApi.postNotification({
        authorization: 'Bearer mock-device-token',
        idempotencyKey: 'event-strong',
        body: strongIntent,
    });
    if (replayedStrong.deliveries[0]?.deliveryId !== strongDelivery.deliveryId) {
        throw new Error('Identical notification replay did not return its original result');
    }
    let conflictingReplayRejected = false;
    try {
        await gateway.deviceApi.postNotification({
            authorization: 'Bearer mock-device-token',
            idempotencyKey: 'event-strong',
            body: { ...strongIntent, reminderType: 'weak', actions: [] },
        });
    } catch (error) {
        conflictingReplayRejected = error instanceof ImGatewayError && error.code === 'idempotency_conflict';
    }
    if (!conflictingReplayRejected) {
        throw new Error('Conflicting notification replay was not rejected');
    }
    const actionToken = await gateway.application.actionUi.issue(strongDelivery.deliveryId);
    await gateway.application.deliveryDispatch.dispatch(strongDelivery.deliveryId);
    await gateway.application.receipts.record({
        externalEventId: 'receipt-delivered',
        channelAccountId: channel.id,
        externalMessageId: 'mock-platform-message',
        dedupeKey: 'wechat-main:receipt-delivered',
        stage: 'delivered',
        occurredAt: '2026-08-03T00:01:00.000Z' as IsoDateTime,
    });
    await gateway.application.receipts.record({
        externalEventId: 'receipt-late-failure',
        channelAccountId: channel.id,
        externalMessageId: 'mock-platform-message',
        dedupeKey: 'wechat-main:receipt-late-failure',
        stage: 'failed',
        occurredAt: '2026-08-03T00:00:30.000Z' as IsoDateTime,
    });
    const delivered = await gateway.application.deliveries.find(strongDelivery.deliveryId);
    if (delivered?.delivery.status !== 'delivered') {
        throw new Error('Late failed receipt moved Delivery backwards');
    }
    const renderedPayload = delivered.attempts[0]?.renderedPayload;
    if (
        typeof renderedPayload !== 'object' ||
        renderedPayload === null ||
        Array.isArray(renderedPayload) ||
        typeof renderedPayload.actionUi !== 'object' ||
        renderedPayload.actionUi === null ||
        Array.isArray(renderedPayload.actionUi) ||
        renderedPayload.actionUi.token !== actionToken
    ) {
        throw new Error('Delivery Renderer did not receive the stable Action UI token');
    }

    const actionView = await gateway.actionUiApi.get(actionToken);
    if (!actionView.actions.includes('snooze')) {
        throw new Error('Action UI token did not resolve Delivery actions');
    }
    const action = await gateway.actionUiApi.post({
        token: actionToken,
        action: 'snooze',
        params: { minutes: 5 },
    });
    const nativeDuplicate = await gateway.application.platformEvents.postEvent({
        id: unsafeId<InboundEventId>('inbound-native-action'),
        externalEventId: 'native-action-event',
        platform: 'wechat_official',
        channelAccountId: channel.id,
        externalIdentityId: binding.externalIdentityId,
        type: 'action.triggered',
        occurredAt: '2026-08-03T00:02:00.000Z' as IsoDateTime,
        payload: { token: actionToken, action: 'snooze', params: { minutes: 5 } },
    });
    if (nativeDuplicate?.commandId !== action.commandId) {
        throw new Error('H5 and native action entries did not converge idempotently');
    }
    const actionCommands = await gateway.actionStreamApi.connect({
        authorization: 'Bearer mock-device-token',
        deviceId,
        reminderType: 'strong',
        reminderTriggerId: action.reminderTriggerId,
    });
    let streamedCommandId: ActionId | undefined;
    for await (const event of actionCommands) {
        streamedCommandId = event.id;
        if (event.event !== 'reminder.action' || event.data.params?.minutes !== 5) {
            throw new Error('SSE reminder.action envelope is incorrect');
        }
        break;
    }
    if (streamedCommandId !== action.commandId) {
        throw new Error('Strong reminder command was not emitted through SSE');
    }
    await gateway.deviceApi.postReminderActionResult({
        authorization: 'Bearer mock-device-token',
        deviceId,
        commandId: action.commandId,
        body: {
            schemaVersion: DEVICE_CONTRACT_VERSION,
            operationId: action.operationId,
            reminderTriggerId: action.reminderTriggerId,
            status: 'retryable_failed',
            occurredAt: '2026-08-03T00:02:00.000Z' as IsoDateTime,
        },
    });
    const replayedCommands = await gateway.actionStreamApi.connect({
        authorization: 'Bearer mock-device-token',
        deviceId,
        reminderType: 'strong',
        reminderTriggerId: action.reminderTriggerId,
        lastEventId: action.commandId,
    });
    let replayedCommandId: ActionId | undefined;
    for await (const event of replayedCommands) {
        replayedCommandId = event.id;
        break;
    }
    if (replayedCommandId !== action.commandId) {
        throw new Error('Unconfirmed command was not replayed after Last-Event-ID');
    }
    const completedAction = await gateway.deviceApi.postReminderActionResult({
        authorization: 'Bearer mock-device-token',
        deviceId,
        commandId: action.commandId,
        body: {
            schemaVersion: DEVICE_CONTRACT_VERSION,
            operationId: action.operationId,
            reminderTriggerId: action.reminderTriggerId,
            status: 'succeeded',
            nextTriggerAt: '2026-08-03T00:07:00.000Z' as IsoDateTime,
            occurredAt: '2026-08-03T00:02:01.000Z' as IsoDateTime,
        },
    });
    if (completedAction.status !== 'succeeded' || completedAction.result?.nextTriggerAt === undefined) {
        throw new Error('Reminder action result was not persisted');
    }

    const anotherChannel = await gateway.application.channels.register({
        platform: 'wechat_official',
        tenantExternalId: 'wechat-app-another',
        koishiBotId: 'wechat-another',
        credentialRef: 'secret://wechat-another',
        connectionMode: 'webhook',
    });
    const commonEvent = {
        id: unsafeId<InboundEventId>('inbound-one'),
        externalEventId: 'same-external-event',
        platform: 'wechat_official' as const,
        type: 'message.received' as const,
        occurredAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
        payload: { text: 'binding only' },
    };
    if (
        (await gateway.application.inboundEvents.recordIfNew({
            ...commonEvent,
            channelAccountId: channel.id,
        })) !== 'accepted' ||
        (await gateway.application.inboundEvents.recordIfNew({
            ...commonEvent,
            id: unsafeId<InboundEventId>('inbound-two'),
            channelAccountId: anotherChannel.id,
        })) !== 'accepted' ||
        (await gateway.application.inboundEvents.recordIfNew({
            ...commonEvent,
            id: unsafeId<InboundEventId>('inbound-three'),
            channelAccountId: channel.id,
        })) !== 'duplicate'
    ) {
        throw new Error('Inbound event composite idempotency is incorrect');
    }

    const weak = await gateway.deviceApi.postNotification({
        authorization: 'Bearer mock-device-token',
        idempotencyKey: 'event-weak',
        body: {
            schemaVersion: DEVICE_CONTRACT_VERSION,
            businessEventId: unsafeId<EventId>('event-weak'),
            correlationId: unsafeId<CorrelationId>('correlation-weak'),
            kind: 'reminder_due',
            recipient: { userId, deviceId },
            scheduleId: unsafeId<ScheduleId>('schedule-2'),
            taskId: unsafeId<TimerTaskId>('task-weak'),
            instanceId: unsafeId<TimerInstanceId>('instance-weak'),
            reminderTriggerId: unsafeId<ReminderTriggerId>('trigger-weak'),
            reminderType: 'weak',
            content: { title: 'Weak reminder' },
            plannedAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
            triggerAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
            actions: [],
            occurredAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
        },
    });
    if (weak.actionStream !== undefined) {
        throw new Error('Weak reminder must not create an action window');
    }

    const expiring = await gateway.deviceApi.postNotification({
        authorization: 'Bearer mock-device-token',
        idempotencyKey: 'event-expiring',
        body: {
            schemaVersion: DEVICE_CONTRACT_VERSION,
            businessEventId: unsafeId<EventId>('event-expiring'),
            correlationId: unsafeId<CorrelationId>('correlation-expiring'),
            kind: 'reminder_due',
            recipient: { userId, deviceId },
            scheduleId: unsafeId<ScheduleId>('schedule-3'),
            taskId: unsafeId<TimerTaskId>('task-expiring'),
            instanceId: unsafeId<TimerInstanceId>('instance-expiring'),
            reminderTriggerId: unsafeId<ReminderTriggerId>('trigger-expiring'),
            reminderType: 'strong',
            content: { title: 'Expiring reminder' },
            plannedAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
            triggerAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
            actions: [{ kind: 'command', type: 'acknowledge', label: '知道了' }],
            occurredAt: '2026-08-03T00:00:00.000Z' as IsoDateTime,
        },
    });
    const expiringDelivery = expiring.deliveries[0];
    if (expiringDelivery === undefined) throw new Error('Expiring Delivery is missing');
    const expiringToken = await gateway.application.actionUi.issue(expiringDelivery.deliveryId);
    const expiringAction = await gateway.actionUiApi.post({
        token: expiringToken,
        action: 'acknowledge',
    });
    clock.advanceMinutes(11);
    if ((await gateway.application.actions.expireDue()) !== 1) {
        throw new Error('Expired Action was not closed by the expiry worker seam');
    }
    if ((await gateway.application.actions.find(expiringAction.commandId))?.status !== 'expired') {
        throw new Error('Expired Action state was not persisted');
    }

    const anonymousPairing = await gateway.application.pairing.create({
        deviceId,
        expiresInMinutes: 1,
    });
    clock.advanceMinutes(2);
    if ((await gateway.application.pairing.expireDue()) !== 1) {
        throw new Error('Expired PairingSession was not closed');
    }
    if ((await gateway.application.pairing.find(anonymousPairing.session.id))?.status !== 'expired') {
        throw new Error('PairingSession nullable-user expiry state was not persisted');
    }
}
