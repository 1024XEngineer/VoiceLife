import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ImGatewayError, WecomAibotInboundAdapter, WecomAibotWssRuntime } from '../dist/index.js';

class FakeWebSocket {
    sent = [];
    listeners = new Map();
    closeEmits = true;
    terminateCalls = 0;

    addEventListener(type, listener) {
        const listeners = this.listeners.get(type) ?? [];
        listeners.push(listener);
        this.listeners.set(type, listeners);
    }

    send(data) {
        this.sent.push(JSON.parse(data));
    }

    close() {
        if (this.closeEmits) this.emit('close', {});
    }

    terminate() {
        this.terminateCalls += 1;
        this.emit('close', {});
    }

    emit(type, event) {
        for (const listener of this.listeners.get(type) ?? []) listener(event);
    }
}

test('WeCom AI Bot WSS runtime subscribes and posts a normalized single-chat binding event', async (context) => {
    const socket = new FakeWebSocket();
    const events = [];
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async (event) => events.push(event),
        createWebSocket: () => socket,
        nextRequestId: () => 'request-fixture',
    });
    context.after(() => runtime.close());

    runtime.start();
    socket.emit('open', {});
    assert.deepEqual(socket.sent, [
        {
            cmd: 'aibot_subscribe',
            headers: { req_id: 'request-fixture' },
            body: { bot_id: 'bot-fixture', secret: 'secret-fixture' },
        },
    ]);

    socket.emit('message', {
        data: JSON.stringify({ headers: { req_id: 'request-fixture' }, errcode: 0 }),
    });
    socket.emit('message', {
        data: JSON.stringify({
            cmd: 'aibot_msg_callback',
            headers: { req_id: 'request-fixture' },
            body: {
                msgid: 'message-fixture',
                aibotid: 'bot-fixture',
                from: { userid: 'userid-fixture' },
                chattype: 'single',
                msgtype: 'text',
                text: { content: 'bind 123456' },
            },
        }),
    });
    await new Promise((resolve) => globalThis.setTimeout(resolve, 0));

    assert.equal(runtime.healthy, true);
    assert.equal(events.length, 1);
    assert.equal(events[0].id, 'channel-wecom:wecom:message-fixture');
    assert.equal(events[0].type, 'binding.requested');
    assert.deepEqual(events[0].payload, { displayCode: '123456', externalUserId: 'userid-fixture' });
    await runtime.close();
});

test('WeCom AI Bot WSS runtime posts template card click events', async (context) => {
    const socket = new FakeWebSocket();
    const events = [];
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({
            channelAccountId: 'channel-wecom',
            botId: 'bot-fixture',
            resolveExternalIdentityId: async () => 'identity-fixture',
        }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async (event) => events.push(event),
        createWebSocket: () => socket,
        nextRequestId: () => 'request-fixture',
    });
    context.after(() => runtime.close());

    runtime.start();
    socket.emit('open', {});
    socket.emit('message', {
        data: JSON.stringify({ headers: { req_id: 'request-fixture' }, errcode: 0 }),
    });
    socket.emit('message', {
        data: JSON.stringify({
            cmd: 'aibot_event_callback',
            headers: { req_id: 'callback-fixture' },
            body: {
                msgid: 'card-click-fixture',
                create_time: 1_786_665_600,
                aibotid: 'bot-fixture',
                from: { userid: 'userid-fixture' },
                chattype: 'single',
                msgtype: 'event',
                event: {
                    eventtype: 'template_card_event',
                    event_key: 'voicelife-action:v1:v1.token.fixture:acknowledge:',
                },
            },
        }),
    });
    await new Promise((resolve) => globalThis.setTimeout(resolve, 0));

    assert.equal(events.length, 1);
    assert.equal(events[0].type, 'action.triggered');
    assert.equal(events[0].externalIdentityId, 'identity-fixture');
    assert.deepEqual(events[0].payload, { token: 'v1.token.fixture', action: 'acknowledge' });
    await runtime.close();
});

test('WeCom AI Bot WSS runtime sends heartbeats, reconnects after a close, and stops reconnecting when closed', async (context) => {
    const sockets = [new FakeWebSocket(), new FakeWebSocket()];
    let created = 0;
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => sockets[created++],
        nextRequestId: () => 'request-fixture',
        heartbeatMilliseconds: 1,
        reconnectDelayMilliseconds: 1,
    });
    context.after(() => runtime.close());

    runtime.start();
    sockets[0].emit('open', {});
    sockets[0].emit('message', {
        data: JSON.stringify({ headers: { req_id: 'request-fixture' }, errcode: 0 }),
    });
    await new Promise((resolve) => globalThis.setTimeout(resolve, 5));
    assert.equal(
        sockets[0].sent.some((frame) => frame.cmd === 'ping'),
        true,
    );

    sockets[0].emit('close', {});
    await new Promise((resolve) => globalThis.setTimeout(resolve, 5));
    assert.equal(created, 2);

    await runtime.close();
    await new Promise((resolve) => globalThis.setTimeout(resolve, 5));
    assert.equal(created, 2);
});

test('WeCom AI Bot WSS runtime reconnects when a heartbeat send fails', async (context) => {
    const sockets = [new FakeWebSocket(), new FakeWebSocket()];
    const originalSend = sockets[0].send.bind(sockets[0]);
    let throwOnHeartbeat = false;
    sockets[0].send = (data) => {
        const frame = JSON.parse(data);
        if (throwOnHeartbeat && frame.cmd === 'ping') throw new Error('socket is closing');
        originalSend(data);
    };
    let created = 0;
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => sockets[created++],
        nextRequestId: () => 'request-fixture',
        heartbeatMilliseconds: 1,
        reconnectDelayMilliseconds: 1,
    });
    context.after(() => runtime.close());

    runtime.start();
    sockets[0].emit('open', {});
    sockets[0].emit('message', {
        data: JSON.stringify({ headers: { req_id: 'request-fixture' }, errcode: 0 }),
    });
    throwOnHeartbeat = true;

    await new Promise((resolve) => globalThis.setTimeout(resolve, 5));

    assert.equal(created, 2);
    assert.equal(runtime.healthy, false);
});

test('WeCom AI Bot WSS runtime reconnects after the platform rejects its subscription', async (context) => {
    const sockets = [new FakeWebSocket(), new FakeWebSocket()];
    let created = 0;
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => sockets[created++],
        nextRequestId: () => 'request-fixture',
        reconnectDelayMilliseconds: 1,
    });
    context.after(() => runtime.close());

    runtime.start();
    sockets[0].emit('open', {});
    sockets[0].emit('message', {
        data: JSON.stringify({ headers: { req_id: 'request-fixture' }, errcode: 40003 }),
    });
    await new Promise((resolve) => globalThis.setTimeout(resolve, 5));

    assert.equal(runtime.healthy, false);
    assert.equal(created, 2);
});

test('WeCom AI Bot WSS runtime reconnects when the platform never acknowledges its subscription', async (context) => {
    const sockets = [new FakeWebSocket(), new FakeWebSocket()];
    let created = 0;
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => sockets[created++],
        reconnectDelayMilliseconds: 1,
        subscriptionAckTimeoutMilliseconds: 1,
    });
    context.after(() => runtime.close());

    runtime.start();
    sockets[0].emit('open', {});
    await new Promise((resolve) => globalThis.setTimeout(resolve, 5));

    assert.equal(runtime.healthy, false);
    assert.equal(created, 2);
});

test('WeCom AI Bot WSS runtime waits for its socket close event during shutdown', async () => {
    const socket = new FakeWebSocket();
    socket.closeEmits = false;
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => socket,
        closeGraceMilliseconds: 50,
    });
    runtime.start();

    let closed = false;
    const closing = runtime.close().then(() => {
        closed = true;
    });
    await new Promise((resolve) => globalThis.setTimeout(resolve, 0));
    assert.equal(closed, false);

    socket.emit('close', {});
    await closing;
    assert.equal(socket.terminateCalls, 0);
});

test('WeCom AI Bot WSS runtime terminates an unresponsive socket after the shutdown grace period', async () => {
    const socket = new FakeWebSocket();
    socket.closeEmits = false;
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => socket,
        closeGraceMilliseconds: 1,
    });
    runtime.start();

    await runtime.close();
    assert.equal(socket.terminateCalls, 1);
});

test('WeCom AI Bot WSS runtime sends Markdown over the subscribed connection and maps its acknowledgement', async () => {
    const socket = new FakeWebSocket();
    let requestNo = 0;
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => socket,
        nextRequestId: () => `request-${String(++requestNo)}`,
    });
    runtime.start();
    socket.emit('open', {});
    socket.emit('message', { data: JSON.stringify({ headers: { req_id: 'request-1' }, errcode: 0 }) });

    const sending = runtime.sendMarkdown('userid-fixture', '**提醒**\n请及时处理');
    assert.deepEqual(socket.sent[1], {
        cmd: 'aibot_send_msg',
        headers: { req_id: 'request-2' },
        body: {
            chatid: 'userid-fixture',
            msgtype: 'markdown',
            markdown: { content: '**提醒**\n请及时处理' },
        },
    });
    socket.emit('message', {
        data: JSON.stringify({ headers: { req_id: 'request-2' }, errcode: 0, body: { msgid: 'platform-message-1' } }),
    });
    assert.deepEqual(await sending, { accepted: true, platformMessageId: 'platform-message-1' });

    const rejected = runtime.sendMarkdown('userid-fixture', '再次提醒');
    socket.emit('message', {
        data: JSON.stringify({ headers: { req_id: 'request-3' }, errcode: 45009 }),
    });
    assert.deepEqual(await rejected, { accepted: false, retryable: true, errorCode: 'wecom_aibot_45009' });
    await runtime.close();
});

test('WeCom AI Bot WSS runtime sends a template card over the subscribed connection', async () => {
    const socket = new FakeWebSocket();
    let requestNo = 0;
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => socket,
        nextRequestId: () => `request-${String(++requestNo)}`,
    });
    runtime.start();
    socket.emit('open', {});
    socket.emit('message', { data: JSON.stringify({ headers: { req_id: 'request-1' }, errcode: 0 }) });

    const card = {
        card_type: 'button_interaction',
        main_title: { title: '提醒' },
        button_list: [{ text: '知道了', style: 1, key: 'opaque-action-key' }],
        task_id: 'voicelife-task',
    };
    const sending = runtime.sendTemplateCard('userid-fixture', card);
    assert.deepEqual(socket.sent[1], {
        cmd: 'aibot_send_msg',
        headers: { req_id: 'request-2' },
        body: { chatid: 'userid-fixture', msgtype: 'template_card', template_card: card },
    });
    socket.emit('message', {
        data: JSON.stringify({ headers: { req_id: 'request-2' }, errcode: 0, body: { msgid: 'platform-card-1' } }),
    });
    assert.deepEqual(await sending, { accepted: true, platformMessageId: 'platform-card-1' });
    await runtime.close();
});

test('WeCom AI Bot WSS runtime rejects invalid or unavailable outbound messages without writing frames', async () => {
    const socket = new FakeWebSocket();
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => socket,
    });
    assert.deepEqual(await runtime.sendMarkdown('userid-fixture', 'message'), {
        accepted: false,
        retryable: true,
        errorCode: 'wecom_aibot_unavailable',
    });
    assert.deepEqual(await runtime.sendMarkdown('userid-fixture', '   '), {
        accepted: false,
        retryable: false,
        errorCode: 'wecom_aibot_invalid_message',
    });
    assert.equal(socket.sent.length, 0);
});

test('WeCom AI Bot WSS runtime validates construction, template cards, and outbound acknowledgement timeouts', async () => {
    assert.throws(
        () =>
            new WecomAibotWssRuntime({
                adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
                botId: ' ',
                secret: 'secret-fixture',
                postEvent: async () => {},
            }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );

    const socket = new FakeWebSocket();
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => socket,
        nextRequestId: () => 'request-fixture',
        outboundAckTimeoutMilliseconds: 1,
    });
    assert.deepEqual(await runtime.sendTemplateCard('userid-fixture', null), {
        accepted: false,
        retryable: false,
        errorCode: 'wecom_aibot_invalid_message',
    });
    assert.deepEqual(await runtime.sendTemplateCard(' '.repeat(513), { card_type: 'button_interaction' }), {
        accepted: false,
        retryable: false,
        errorCode: 'wecom_aibot_invalid_message',
    });

    runtime.start();
    socket.emit('open', {});
    socket.emit('message', { data: JSON.stringify({ headers: { req_id: 'request-fixture' }, errcode: 0 }) });
    assert.deepEqual(await runtime.sendMarkdown('userid-fixture', 'timeout'), {
        accepted: false,
        retryable: true,
        errorCode: 'wecom_aibot_timeout',
    });
    await runtime.close();
});

test('WeCom AI Bot WSS runtime ignores malformed platform frames and callback failures', async () => {
    const socket = new FakeWebSocket();
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {
            throw new Error('application rejected event');
        },
        createWebSocket: () => socket,
        nextRequestId: () => 'request-fixture',
    });
    runtime.start();
    socket.emit('open', {});
    socket.emit('message', { data: 'not-json' });
    socket.emit('message', { data: JSON.stringify([]) });
    socket.emit('message', { data: JSON.stringify({ headers: { req_id: 'request-fixture' }, errcode: 0 }) });
    socket.emit('message', {
        data: JSON.stringify({ cmd: 'aibot_msg_callback', body: { msgid: 'bad-callback' } }),
    });
    await new Promise((resolve) => globalThis.setTimeout(resolve, 0));
    assert.equal(runtime.healthy, true);
    await runtime.close();
});

test('WeCom AI Bot WSS runtime classifies transport errors and pending sends on disconnect as retryable', async (context) => {
    const socket = new FakeWebSocket();
    const originalSend = socket.send.bind(socket);
    let shouldThrow = false;
    socket.send = (data) => {
        if (shouldThrow) throw new Error('socket is closed');
        originalSend(data);
    };
    let requestNo = 0;
    const runtime = new WecomAibotWssRuntime({
        adapter: new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' }),
        botId: 'bot-fixture',
        secret: 'secret-fixture',
        postEvent: async () => {},
        createWebSocket: () => socket,
        nextRequestId: () => `request-${String(++requestNo)}`,
    });
    context.after(() => runtime.close());

    runtime.start();
    socket.emit('open', {});
    socket.emit('message', { data: JSON.stringify({ headers: { req_id: 'request-1' }, errcode: 0 }) });

    shouldThrow = true;
    assert.deepEqual(await runtime.sendMarkdown('userid-fixture', 'transport failure'), {
        accepted: false,
        retryable: true,
        errorCode: 'wecom_aibot_transport_error',
    });

    shouldThrow = false;
    const pending = runtime.sendMarkdown('userid-fixture', 'pending request');
    socket.emit('close', {});
    assert.deepEqual(await pending, {
        accepted: false,
        retryable: true,
        errorCode: 'wecom_aibot_unavailable',
    });
});
