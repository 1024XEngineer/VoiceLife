import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ChannelAdapterRegistry, ImGatewayError, WecomAibotInboundAdapter } from '../dist/index.js';

function adapter(overrides = {}) {
    return new WecomAibotInboundAdapter({
        channelAccountId: 'channel-wecom',
        botId: 'bot-fixture',
        now: () => '2026-08-18T00:00:00.000Z',
        ...overrides,
    });
}

function textFrame(overrides = {}) {
    return {
        msgid: 'message-fixture',
        aibotid: 'bot-fixture',
        from: { userid: 'userid-fixture' },
        chattype: 'single',
        msgtype: 'text',
        text: { content: '绑定 123456' },
        create_time: 1_786_665_600,
        ...overrides,
    };
}

test('WeCom AI Bot normalizes a single-chat binding text with userid and msgid', async () => {
    const event = await adapter().normalizeInbound(textFrame());

    assert.deepEqual(event, {
        id: 'channel-wecom:wecom:message-fixture',
        externalEventId: 'message-fixture',
        platform: 'wecom_aibot',
        channelAccountId: 'channel-wecom',
        occurredAt: '2026-08-14T00:00:00.000Z',
        type: 'binding.requested',
        payload: { displayCode: '123456', externalUserId: 'userid-fixture' },
    });
});

test('WeCom AI Bot preserves ordinary single-chat text as a message event', async () => {
    const event = await adapter().normalizeInbound(
        textFrame({ msgid: 'message-ordinary', text: { content: 'hello' } }),
    );

    assert.deepEqual(event, {
        id: 'channel-wecom:wecom:message-ordinary',
        externalEventId: 'message-ordinary',
        platform: 'wecom_aibot',
        channelAccountId: 'channel-wecom',
        occurredAt: '2026-08-14T00:00:00.000Z',
        type: 'message.received',
        payload: { externalUserId: 'userid-fixture', messageType: 'text', text: 'hello' },
    });
});

test('WeCom AI Bot normalizes a template card click into an action event and resolves its identity', async () => {
    const wecom = adapter({
        resolveExternalIdentityId: async (externalUserId) =>
            externalUserId === 'userid-fixture' ? 'identity-fixture' : undefined,
    });
    const event = await wecom.normalizeInbound({
        ...textFrame({ msgid: 'card-click-fixture', msgtype: 'event' }),
        event: {
            eventtype: 'template_card_event',
            template_card_event: {
                event_key: 'voicelife-action:v1:v1.token.fixture:snooze:10',
                task_id: 'voicelife-task',
            },
        },
    });

    assert.deepEqual(event, {
        id: 'channel-wecom:wecom:card-click-fixture',
        externalEventId: 'card-click-fixture',
        platform: 'wecom_aibot',
        channelAccountId: 'channel-wecom',
        externalIdentityId: 'identity-fixture',
        occurredAt: '2026-08-14T00:00:00.000Z',
        type: 'action.triggered',
        payload: {
            token: 'v1.token.fixture',
            action: 'snooze',
            params: { minutes: 10 },
        },
    });
});

test('WeCom AI Bot uses its receive time when a valid single-chat message omits create_time', async () => {
    const frame = textFrame({ msgid: 'message-without-time' });
    delete frame.create_time;

    const event = await adapter().normalizeInbound(frame);

    assert.equal(event.occurredAt, '2026-08-18T00:00:00.000Z');
});

test('WeCom AI Bot resolves registered active accounts as unavailable for outbound delivery', async () => {
    const registry = new ChannelAdapterRegistry([{ accountId: 'channel-wecom', adapter: adapter() }]);

    assert.deepEqual(await registry.resolve({ id: 'channel-wecom', platform: 'wecom_aibot', status: 'active' }), {
        proactiveMessage: false,
        nativeAction: false,
        actionUi: false,
        deliveryReceipt: false,
        presentationTypes: [],
    });
});

test('WeCom AI Bot renders Markdown and sends it through the injected WSS transport', async () => {
    const sent = [];
    const wecom = adapter({
        outbound: {
            revealExternalUserId: async (ciphertext) => ciphertext.replace('encrypted:', ''),
            transport: {
                sendMarkdown: async (chatId, content) => {
                    sent.push({ chatId, content });
                    return { accepted: true, platformMessageId: 'platform-message-1' };
                },
            },
        },
    });
    assert.deepEqual(await wecom.resolve({ id: 'channel-wecom', platform: 'wecom_aibot', status: 'active' }), {
        proactiveMessage: true,
        nativeAction: false,
        actionUi: false,
        deliveryReceipt: false,
        presentationTypes: ['rich_text'],
    });
    assert.deepEqual(await wecom.renderNotification({ content: { title: '提醒', body: '该处理了' } }), {
        type: 'wecom_aibot_markdown',
        content: '**提醒**\n该处理了',
    });
    assert.deepEqual(
        await wecom.send({
            delivery: { channelAccountId: 'channel-wecom' },
            conversation: { externalConversationIdCiphertext: 'encrypted:userid-fixture' },
            content: { type: 'wecom_aibot_markdown', content: '**提醒**\n该处理了' },
        }),
        { accepted: true, platformMessageId: 'platform-message-1' },
    );
    assert.deepEqual(sent, [{ chatId: 'userid-fixture', content: '**提醒**\n该处理了' }]);
});

test('WeCom AI Bot renders a strong reminder as a native button card with opaque action keys', async () => {
    const wecom = adapter({
        outbound: {
            revealExternalUserId: async (ciphertext) => ciphertext.replace('encrypted:', ''),
            transport: {
                sendMarkdown: async () => ({ accepted: true }),
                sendTemplateCard: async () => ({ accepted: true }),
            },
        },
    });
    const capabilities = await wecom.resolve({ id: 'channel-wecom', platform: 'wecom_aibot', status: 'active' });
    assert.deepEqual(capabilities, {
        proactiveMessage: true,
        nativeAction: true,
        actionUi: false,
        deliveryReceipt: false,
        presentationTypes: ['native_card', 'rich_text'],
    });
    const rendered = await wecom.render(
        {
            channelAccountId: 'channel-wecom',
            presentationType: 'native_card',
            kind: 'reminder_due',
            semanticPayload: {
                schemaVersion: '1',
                businessEventId: 'event-fixture',
                correlationId: 'correlation-fixture',
                kind: 'reminder_due',
                reminderType: 'strong',
                reminderTriggerId: 'trigger-fixture',
                recipient: { userId: 'user-fixture', deviceId: 'device-fixture' },
                scheduleId: 'schedule-fixture',
                taskId: 'task-fixture',
                instanceId: 'instance-fixture',
                actions: [
                    { kind: 'command', type: 'acknowledge', label: '知道了' },
                    { kind: 'command', type: 'snooze', label: '推迟 10 分钟', params: { minutes: 10 } },
                ],
                content: { title: '提醒', body: '该处理了' },
                plannedAt: '2026-08-03T00:00:00.000Z',
                triggerAt: '2026-08-03T00:00:00.000Z',
                occurredAt: '2026-08-03T00:00:00.000Z',
            },
        },
        { id: 'channel-wecom', platform: 'wecom_aibot', status: 'active' },
        capabilities,
        { actionToken: 'v1.token.fixture' },
    );
    assert.deepEqual(rendered, {
        type: 'wecom_aibot_template_card',
        template_card: {
            card_type: 'button_interaction',
            main_title: { title: '提醒' },
            sub_title_text: '该处理了',
            button_list: [
                {
                    text: '知道了',
                    style: 1,
                    key: 'voicelife-action:v1:v1.token.fixture:acknowledge:',
                },
                {
                    text: '推迟 10 分钟',
                    style: 2,
                    key: 'voicelife-action:v1:v1.token.fixture:snooze:10',
                },
            ],
            task_id: 'voicelife-509c969756ec9a9bba4c963f',
        },
    });
    assert.deepEqual(
        await wecom.send({
            delivery: { channelAccountId: 'channel-wecom' },
            conversation: { externalConversationIdCiphertext: 'encrypted:userid-fixture' },
            content: rendered,
        }),
        { accepted: true },
    );
});

test('WeCom AI Bot refuses a mismatched delivery before revealing the recipient', async () => {
    const wecom = adapter({
        outbound: {
            revealExternalUserId: async () => {
                throw new Error('recipient must not be revealed');
            },
            transport: { sendMarkdown: async () => ({ accepted: true }) },
        },
    });
    assert.deepEqual(
        await wecom.send({
            delivery: { channelAccountId: 'another-channel' },
            conversation: { externalConversationIdCiphertext: 'encrypted:userid-fixture' },
            content: { type: 'wecom_aibot_markdown', content: 'message' },
        }),
        { accepted: false, retryable: false, errorCode: 'wecom_aibot_account_mismatch' },
    );
});

test('WeCom AI Bot rejects malformed outbound content before revealing the recipient', async () => {
    let revealed = false;
    const wecom = adapter({
        outbound: {
            revealExternalUserId: async () => {
                revealed = true;
                return 'userid-fixture';
            },
            transport: { sendMarkdown: async () => ({ accepted: true }) },
        },
    });
    assert.deepEqual(
        await wecom.send({
            delivery: { channelAccountId: 'channel-wecom' },
            conversation: { externalConversationIdCiphertext: 'encrypted:userid-fixture' },
            content: { type: 'unknown' },
        }),
        { accepted: false, retryable: false, errorCode: 'wecom_aibot_invalid_message' },
    );
    assert.equal(revealed, false);
});

test('WeCom AI Bot rejects a message for another bot, an empty userid, and group context', async () => {
    for (const frame of [
        textFrame({ aibotid: 'bot-other' }),
        textFrame({ from: { userid: '  ' } }),
        textFrame({ chattype: 'group', chatid: 'chat-fixture' }),
        textFrame({ chatid: 'chat-fixture' }),
    ]) {
        await assert.rejects(
            () => adapter().normalizeInbound(frame),
            (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
        );
    }
});

test('WeCom AI Bot makes unconfigured outbound operations explicitly unavailable', async () => {
    const wecom = adapter();
    const account = { id: 'channel-wecom', platform: 'wecom_aibot', status: 'active' };

    assert.deepEqual(await wecom.resolve(account), {
        proactiveMessage: false,
        nativeAction: false,
        actionUi: false,
        deliveryReceipt: false,
        presentationTypes: [],
    });
    assert.deepEqual(
        await wecom.send({
            delivery: { channelAccountId: 'channel-wecom' },
            conversation: { externalConversationIdCiphertext: 'encrypted:userid-fixture' },
            content: { type: 'wecom_aibot_markdown', content: '提醒' },
        }),
        { accepted: false, retryable: false, errorCode: 'wecom_aibot_not_configured' },
    );
    for (const operation of [
        () => wecom.renderScheduleReceipt({ summary: '已更新' }),
        () => wecom.renderNotification({ content: { title: '提醒' } }),
        () =>
            wecom.render(
                { channelAccountId: 'channel-wecom', presentationType: 'rich_text', kind: 'schedule_receipt' },
                account,
                awaitableUnavailableCapabilities(),
                {},
            ),
    ]) {
        await assert.rejects(
            operation,
            (error) => error instanceof ImGatewayError && error.code === 'capability_not_supported',
        );
    }
});

function awaitableUnavailableCapabilities() {
    return {
        proactiveMessage: false,
        nativeAction: false,
        actionUi: false,
        deliveryReceipt: false,
        presentationTypes: [],
    };
}

test('WeCom AI Bot renders receipt text, rejects unsupported native cards, and validates outbound cards', async () => {
    let revealed = false;
    const wecom = adapter({
        outbound: {
            revealExternalUserId: async () => {
                revealed = true;
                return 'userid-fixture';
            },
            transport: { sendMarkdown: async () => ({ accepted: true }) },
        },
    });
    const account = { id: 'channel-wecom', platform: 'wecom_aibot', status: 'active' };
    const capabilities = await wecom.resolve(account);

    assert.deepEqual(await wecom.renderScheduleReceipt({ summary: '  已更新  ' }), {
        type: 'wecom_aibot_markdown',
        content: '**日程已更新**\n已更新',
    });
    assert.deepEqual(await wecom.renderNotification({ content: { title: '  提醒  ', body: '  处理它  ' } }), {
        type: 'wecom_aibot_markdown',
        content: '**提醒**\n处理它',
    });
    await assert.rejects(
        () =>
            wecom.render(
                {
                    channelAccountId: 'channel-wecom',
                    presentationType: 'native_card',
                    kind: 'reminder_due',
                    semanticPayload: { reminderType: 'weak', actions: [] },
                },
                account,
                capabilities,
                { actionToken: 'v1.token.fixture' },
            ),
        (error) => error instanceof ImGatewayError && error.code === 'capability_not_supported',
    );
    assert.deepEqual(
        await wecom.send({
            delivery: { channelAccountId: 'channel-wecom' },
            conversation: { externalConversationIdCiphertext: 'encrypted:userid-fixture' },
            content: { type: 'wecom_aibot_template_card', template_card: { card_type: 'button_interaction' } },
        }),
        { accepted: false, retryable: false, errorCode: 'wecom_aibot_invalid_message' },
    );
    assert.equal(revealed, true);
});

test('WeCom AI Bot rejects malformed events before they become action commands', async () => {
    const invalidFrames = [
        textFrame({ msgtype: 'image' }),
        textFrame({ text: { content: 'x'.repeat(16 * 1024 + 1) } }),
        textFrame({ msgtype: 'event', event: { eventtype: 'other' } }),
        textFrame({ msgtype: 'event', event: { eventtype: 'template_card_event' } }),
        textFrame({
            msgtype: 'event',
            event: { eventtype: 'template_card_event', event_key: 'invalid' },
        }),
    ];
    for (const frame of invalidFrames) {
        await assert.rejects(
            () => adapter().normalizeInbound(frame),
            (error) => error instanceof ImGatewayError,
        );
    }

    const actionFrame = textFrame({
        msgtype: 'event',
        event: {
            eventtype: 'template_card_event',
            event_key: 'voicelife-action:v1:v1.token.fixture:acknowledge:',
        },
    });
    await assert.rejects(
        () => adapter().normalizeInbound(actionFrame),
        (error) => error instanceof ImGatewayError && error.code === 'capability_not_supported',
    );
    await assert.rejects(
        () => adapter({ resolveExternalIdentityId: async () => undefined }).normalizeInbound(actionFrame),
        (error) => error instanceof ImGatewayError && error.code === 'action_expired',
    );
});
