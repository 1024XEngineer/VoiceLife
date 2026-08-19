import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ImGatewayError, WecomAibotInboundAdapter } from '../dist/index.js';

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

test('WeCom AI Bot uses its receive time when a valid single-chat message omits create_time', async () => {
    const frame = textFrame({ msgid: 'message-without-time' });
    delete frame.create_time;

    const event = await adapter().normalizeInbound(frame);

    assert.equal(event.occurredAt, '2026-08-18T00:00:00.000Z');
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
