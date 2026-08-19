import assert from 'node:assert/strict';
import { test } from 'node:test';

import { WecomAibotInboundAdapter, WecomAibotWssRuntime } from '../dist/index.js';

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

test('WeCom AI Bot WSS runtime subscribes and posts a normalized single-chat binding event', async () => {
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
