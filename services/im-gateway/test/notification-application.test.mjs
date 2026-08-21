import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    bindFixtureUser,
    buildGateway,
    expectGatewayError,
    fixedCapabilities,
    scheduleReceiptIntent,
    scheduleQueryResultIntent,
    seedDevice,
    strongIntent,
    weakIntent,
} from './helpers.mjs';

test('schedule query result preserves all entries and is idempotent', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);

    const intent = scheduleQueryResultIntent();
    const first = await gateway.application.notifications.submitScheduleQueryResult(intent);
    const replay = await gateway.application.notifications.submitScheduleQueryResult(intent);
    assert.equal(first.deliveries.length, 1);
    assert.deepEqual(replay, first);
    const details = await gateway.application.deliveries.find(first.deliveries[0].deliveryId);
    assert.equal(details.delivery.kind, 'schedule_query_result');
    assert.deepEqual(details.delivery.semanticPayload.schedules, intent.schedules);
    assert.deepEqual(details.delivery.semanticPayload.futureOccurrences, intent.futureOccurrences);
    assert.deepEqual(details.delivery.semanticPayload.exceptions, intent.exceptions);
});

test('strong reminder creates a pending delivery with an action stream', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitNotification(strongIntent());

    assert.equal(submission.deliveries.length, 1);
    assert.equal(submission.actionStream.reminderTriggerId, 'trigger-fixture');
    const details = await gateway.application.deliveries.find(submission.deliveries[0].deliveryId);
    assert.equal(details.delivery.kind, 'reminder_due');
    assert.equal(details.delivery.status, 'pending');
});

test('weak reminder creates a delivery without an action stream', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitNotification(weakIntent());

    assert.equal(submission.deliveries.length, 1);
    assert.equal(submission.actionStream, undefined);
});

test('native-action channel selects native_card presentation', async () => {
    const { gateway } = buildGateway({
        channelCapabilities: fixedCapabilities({
            proactiveMessage: true,
            nativeAction: true,
            actionUi: true,
            deliveryReceipt: true,
            presentationTypes: ['native_card', 'template', 'text_with_action_ui'],
        }),
    });
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    const details = await gateway.application.deliveries.find(submission.deliveries[0].deliveryId);

    assert.equal(details.delivery.presentationType, 'native_card');
});

test('channel without native action but with template selects template', async () => {
    const { gateway } = buildGateway({
        channelCapabilities: fixedCapabilities({
            proactiveMessage: true,
            nativeAction: false,
            actionUi: true,
            deliveryReceipt: true,
            presentationTypes: ['template', 'text_with_action_ui'],
        }),
    });
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    const details = await gateway.application.deliveries.find(submission.deliveries[0].deliveryId);

    assert.equal(details.delivery.presentationType, 'template');
});

test('channel with only action UI selects text_with_action_ui for strong reminders', async () => {
    const { gateway } = buildGateway({
        channelCapabilities: fixedCapabilities({
            proactiveMessage: true,
            nativeAction: false,
            actionUi: true,
            deliveryReceipt: true,
            presentationTypes: ['text_with_action_ui'],
        }),
    });
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    const details = await gateway.application.deliveries.find(submission.deliveries[0].deliveryId);

    assert.equal(details.delivery.presentationType, 'text_with_action_ui');
});

test('weak reminder on a rich-text channel selects rich_text', async () => {
    const { gateway } = buildGateway({
        channelCapabilities: fixedCapabilities({
            proactiveMessage: true,
            nativeAction: false,
            actionUi: false,
            deliveryReceipt: false,
            presentationTypes: ['rich_text'],
        }),
    });
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitNotification(weakIntent());
    const details = await gateway.application.deliveries.find(submission.deliveries[0].deliveryId);

    assert.equal(details.delivery.presentationType, 'rich_text');
});

test('strong reminder prefers rich text with H5 over plain text with H5', async () => {
    const { gateway } = buildGateway({
        channelCapabilities: fixedCapabilities({
            proactiveMessage: true,
            nativeAction: false,
            actionUi: true,
            deliveryReceipt: false,
            presentationTypes: ['rich_text', 'text_with_action_ui'],
        }),
    });
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitNotification(strongIntent());
    const details = await gateway.application.deliveries.find(submission.deliveries[0].deliveryId);

    assert.equal(details.delivery.presentationType, 'rich_text');
});

test('strong reminder without a native or H5 action entry produces no delivery', async () => {
    const { gateway } = buildGateway({
        channelCapabilities: fixedCapabilities({
            proactiveMessage: true,
            nativeAction: false,
            actionUi: false,
            deliveryReceipt: false,
            presentationTypes: ['rich_text'],
        }),
    });
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitNotification(strongIntent());

    assert.equal(submission.deliveries.length, 0);
    assert.equal(submission.actionStream, undefined);
});

test('channel without proactive messaging produces no delivery', async () => {
    const { gateway } = buildGateway({
        channelCapabilities: fixedCapabilities({
            proactiveMessage: false,
            nativeAction: true,
            actionUi: true,
            deliveryReceipt: true,
            presentationTypes: ['native_card', 'template', 'rich_text', 'text_with_action_ui'],
        }),
    });
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitNotification(strongIntent());

    assert.equal(submission.deliveries.length, 0);
    assert.equal(submission.actionStream, undefined);
});

test('notification with no binding produces no delivery', async () => {
    const { gateway } = buildGateway();

    const submission = await gateway.application.notifications.submitNotification(strongIntent());

    assert.equal(submission.deliveries.length, 0);
    assert.equal(submission.actionStream, undefined);
});

test('notification to a disabled channel account produces no delivery', async () => {
    const { gateway } = buildGateway();
    const { channel } = await bindFixtureUser(gateway);
    await gateway.application.channels.disable(channel.id);

    const submission = await gateway.application.notifications.submitNotification(
        strongIntent({ businessEventId: 'event-after-disable' }),
    );

    assert.equal(submission.deliveries.length, 0);
});

test('notification after unbind produces no new delivery', async () => {
    const { gateway } = buildGateway();
    const { binding } = await bindFixtureUser(gateway);
    await gateway.application.bindings.unbind(binding.id);

    const submission = await gateway.application.notifications.submitNotification(
        strongIntent({ businessEventId: 'event-after-unbind' }),
    );

    assert.equal(submission.deliveries.length, 0);
});

test('notification after revoke produces no new delivery', async () => {
    const { gateway } = buildGateway();
    const { binding } = await bindFixtureUser(gateway);
    await gateway.application.bindings.revoke(binding.id);

    const submission = await gateway.application.notifications.submitNotification(
        strongIntent({ businessEventId: 'event-after-revoke' }),
    );

    assert.equal(submission.deliveries.length, 0);
});

test('notification filters deliveries to the intent device binding', async () => {
    const { gateway, unitOfWork } = buildGateway();
    seedDevice(unitOfWork, 'device-a', 'user-fixture', 2);
    seedDevice(unitOfWork, 'device-b', 'user-fixture', 3);
    const first = await bindFixtureUser(gateway, {
        userId: 'user-fixture',
        deviceId: 'device-a',
        externalUserId: 'open-a',
    });
    await bindFixtureUser(gateway, { userId: 'user-fixture', deviceId: 'device-b', externalUserId: 'open-b' });

    const submission = await gateway.application.notifications.submitNotification(
        strongIntent({
            businessEventId: 'event-device-filter',
            recipient: { userId: 'user-fixture', deviceId: 'device-a' },
        }),
    );

    assert.equal(submission.deliveries.length, 1);
    assert.equal(submission.deliveries[0].bindingId, first.binding.id);
});

test('identical replay returns the original submission', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);

    const first = await gateway.application.notifications.submitNotification(strongIntent());
    const replay = await gateway.application.notifications.submitNotification(strongIntent());

    assert.equal(replay.deliveries[0].deliveryId, first.deliveries[0].deliveryId);
});

test('conflicting replay of the same business event is rejected', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);

    await gateway.application.notifications.submitNotification(strongIntent());
    await expectGatewayError(
        () =>
            gateway.application.notifications.submitNotification(
                strongIntent({ content: { title: 'Different fixture reminder' } }),
            ),
        'idempotency_conflict',
        'Conflicting content with the same business event ID was accepted',
    );
});

test('schedule receipt creates a schedule_receipt delivery without an action stream', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);

    const submission = await gateway.application.notifications.submitScheduleReceipt(scheduleReceiptIntent());

    assert.equal(submission.deliveries.length, 1);
    assert.equal(submission.actionStream, undefined);
    const details = await gateway.application.deliveries.find(submission.deliveries[0].deliveryId);
    assert.equal(details.delivery.kind, 'schedule_receipt');
    assert.equal(details.delivery.status, 'pending');
});

test('schedule receipt without userId resolves bindings by device', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);

    const intent = scheduleReceiptIntent();
    delete intent.userId;
    const submission = await gateway.application.notifications.submitScheduleReceipt(intent);

    assert.equal(submission.deliveries.length, 1);
});

test('identical schedule receipt replay returns the original submission', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);

    const first = await gateway.application.notifications.submitScheduleReceipt(scheduleReceiptIntent());
    const replay = await gateway.application.notifications.submitScheduleReceipt(scheduleReceiptIntent());

    assert.equal(replay.deliveries[0].deliveryId, first.deliveries[0].deliveryId);
});

test('conflicting schedule receipt replay is rejected', async () => {
    const { gateway } = buildGateway();
    await bindFixtureUser(gateway);
    await gateway.application.notifications.submitScheduleReceipt(scheduleReceiptIntent());

    await expectGatewayError(
        () =>
            gateway.application.notifications.submitScheduleReceipt(
                scheduleReceiptIntent({ summary: 'different result summary' }),
            ),
        'idempotency_conflict',
        'A conflicting schedule receipt replay was accepted',
    );
});
