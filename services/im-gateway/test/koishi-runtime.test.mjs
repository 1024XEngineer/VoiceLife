import assert from 'node:assert/strict';
import { test } from 'node:test';

import { Context, HTTP } from '@koishijs/core';

import { VoiceLifeKoishiPlugin, createKoishiGatewayRuntime, mockImGatewayPorts } from '../dist/index.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';

test('VoiceLife Koishi plugin receives real Context events and calls Application directly', async () => {
    const context = new Context();
    const posted = [];
    const plugin = new VoiceLifeKoishiPlugin(
        context,
        {
            platform: 'wechat_official',
            capabilities: async () => ({}),
            renderScheduleReceipt: async () => ({}),
            renderNotification: async () => ({}),
            normalizeInbound: async (rawEvent) => ({
                id: `event-${rawEvent.messageId}`,
                externalEventId: rawEvent.messageId,
                platform: 'wechat_official',
                channelAccountId: 'channel-fixture',
                occurredAt: '2026-08-03T00:00:00.000Z',
                type: 'message.received',
                payload: { text: rawEvent.content },
            }),
        },
        { postEvent: async (event) => posted.push(event) },
    );

    plugin.start();
    plugin.start();
    await context.emit({ platform: 'other-platform' }, 'message', {
        platform: 'other-platform',
        messageId: 'message-other-platform',
        content: 'ignored',
    });
    assert.equal(posted.length, 0);
    await context.emit({ platform: 'wechat_official' }, 'message', {
        platform: 'wechat_official',
        messageId: 'message-fixture',
        content: 'bind 1234',
    });
    assert.equal(posted.length, 1);
    assert.equal(posted[0].externalEventId, 'message-fixture');

    plugin.stop();
    await context.emit({ platform: 'wechat_official' }, 'message', {
        platform: 'wechat_official',
        messageId: 'message-after-stop',
        content: 'ignored',
    });
    assert.equal(posted.length, 1);
});

test('Koishi composition root owns a real Context and has idempotent start/close lifecycle', async () => {
    const ports = mockImGatewayPorts();
    const lifecycle = [];
    const context = new Context();
    const contextStart = context.start.bind(context);
    let startCalls = 0;
    context.start = async () => {
        startCalls += 1;
        await Promise.resolve();
        return contextStart();
    };
    context.on('ready', () => lifecycle.push('ready'));
    context.on('dispose', () => lifecycle.push('dispose'));

    const composed = createKoishiGatewayRuntime({
        context,
        dependencies: {
            ...ports,
            unitOfWork: new InMemoryImUnitOfWork(),
            actionStream: undefined,
        },
        capabilities: [],
        revealExternalUserId: async (value) => value,
    });

    assert.equal(composed.context, context);
    assert.ok(composed.runtime.application);
    assert.ok(composed.actionStream);
    await Promise.all([composed.start(), composed.start()]);
    assert.equal(startCalls, 1);
    assert.deepEqual(lifecycle, ['ready']);

    await composed.close();
    await composed.close();
    assert.deepEqual(lifecycle, ['ready', 'dispose']);
    await assert.rejects(
        () => composed.start(),
        (error) => error.code === 'invalid_transition',
    );
});

test('Koishi composition root unregisters plugins when Context startup fails', async () => {
    const context = new Context();
    const expected = new Error('fixture startup failure');
    context.start = async () => {
        throw expected;
    };
    let normalized = 0;
    const ports = mockImGatewayPorts();
    const composed = createKoishiGatewayRuntime({
        context,
        dependencies: { ...ports, unitOfWork: new InMemoryImUnitOfWork() },
        capabilities: [
            {
                platform: 'wechat_official',
                capabilities: async () => ({}),
                renderScheduleReceipt: async () => ({}),
                renderNotification: async () => ({}),
                normalizeInbound: async () => {
                    normalized += 1;
                    return {};
                },
            },
        ],
        revealExternalUserId: async (value) => value,
    });

    await assert.rejects(() => composed.start(), expected);
    await context.emit({ platform: 'wechat_official' }, 'message', {
        platform: 'wechat_official',
        messageId: 'message-after-failure',
    });
    assert.equal(normalized, 0);
});

test('Koishi composition root stops a Context when close races with startup', async () => {
    const context = new Context();
    const contextStart = context.start.bind(context);
    let releaseStart;
    const startupGate = new Promise((resolve) => {
        releaseStart = resolve;
    });
    context.start = async () => {
        await startupGate;
        return contextStart();
    };
    const lifecycle = [];
    context.on('ready', () => lifecycle.push('ready'));
    context.on('dispose', () => lifecycle.push('dispose'));
    const ports = mockImGatewayPorts();
    const composed = createKoishiGatewayRuntime({
        context,
        dependencies: { ...ports, unitOfWork: new InMemoryImUnitOfWork() },
        capabilities: [],
        revealExternalUserId: async (value) => value,
    });

    const starting = composed.start();
    const closing = composed.close();
    releaseStart();
    await Promise.all([starting, closing]);

    assert.deepEqual(lifecycle, ['ready', 'dispose']);
});

test('Koishi HTTP adapter loads local files with the patched file-type API', async () => {
    const context = new Context();
    context.plugin(HTTP);
    await context.start();

    try {
        const loaded = await context.http.file(new URL('../package.json', import.meta.url).href);
        assert.equal(loaded.filename, 'package.json');
        assert.ok(loaded.data.byteLength > 0);
    } finally {
        await context.stop();
    }
});
