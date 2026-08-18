import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ImGatewayError, WecomAibotInboundAdapter } from '../dist/index.js';

function adapter() {
    return new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' });
}

function textFrame(overrides = {}) {
    return {
        cmd: 'aibot_msg_callback',
        headers: { req_id: 'request-fixture' },
        body: {
            msgid: 'message-fixture',
            aibotid: 'bot-fixture',
            from: { userid: 'userid-fixture' },
            chattype: 'single',
            msgtype: 'text',
            text: { content: '绑定 123456' },
            create_time: 1_786_665_600,
            ...overrides,
        },
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

test('WeCom AI Bot rejects a message for another bot, an empty userid, and group chat', async () => {
    for (const frame of [
        textFrame({ aibotid: 'bot-other' }),
        textFrame({ from: { userid: '  ' } }),
        textFrame({ chattype: 'group', chatid: 'chat-fixture' }),
    ]) {
        await assert.rejects(
            () => adapter().normalizeInbound(frame),
            (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
        );
    }
});
