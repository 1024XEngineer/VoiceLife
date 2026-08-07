import assert from 'node:assert/strict';
import { test } from 'node:test';

import { SseActionCommandHub } from '../dist/index.js';

function command(overrides = {}) {
    return {
        schemaVersion: '1',
        commandId: 'action-fixture',
        operationId: 'operation-fixture',
        correlationId: 'correlation-fixture',
        deviceId: 'device-fixture',
        reminderTriggerId: 'trigger-fixture',
        action: 'acknowledge',
        issuedAt: '2026-08-03T00:00:00.000Z',
        expiresAt: '2099-08-03T00:10:00.000Z',
        ...overrides,
    };
}

function subscription(overrides = {}) {
    return {
        deviceId: 'device-fixture',
        reminderTriggerId: 'trigger-fixture',
        expiresAt: '2099-08-03T00:10:00.000Z',
        ...overrides,
    };
}

function iterator(stream) {
    return stream[Symbol.asyncIterator]();
}

test('SSE action hub delivers live commands only to the matching device and reminder window', async () => {
    const hub = new SseActionCommandHub();
    const matching = iterator(hub.subscribe(subscription()));
    const otherDevice = iterator(
        hub.subscribe(subscription({ deviceId: 'device-other', signal: globalThis.AbortSignal.timeout(25) })),
    );
    const otherTrigger = iterator(
        hub.subscribe(subscription({ reminderTriggerId: 'trigger-other', signal: globalThis.AbortSignal.timeout(25) })),
    );

    const next = matching.next();
    await hub.publish(command());

    assert.deepEqual(await next, { done: false, value: command() });
    assert.deepEqual(await otherDevice.next(), { done: true, value: undefined });
    assert.deepEqual(await otherTrigger.next(), { done: true, value: undefined });
    await matching.return();
});

test('SSE action hub does not treat Last-Event-ID as an acknowledgement', async () => {
    const hub = new SseActionCommandHub();
    const stream = iterator(hub.subscribe(subscription({ lastEventId: 'action-fixture' })));

    const replayedRetry = stream.next();
    await hub.publish(command());
    assert.equal((await replayedRetry).value.commandId, 'action-fixture');

    const next = stream.next();
    await hub.publish(command({ commandId: 'action-next', operationId: 'operation-next' }));

    assert.equal((await next).value.commandId, 'action-next');
    await stream.return();
});

test('SSE action hub closes an action scope after result acknowledgement', async () => {
    const hub = new SseActionCommandHub();
    const stream = iterator(hub.subscribe(subscription()));
    const first = stream.next();
    await hub.publish(command());
    assert.equal((await first).value.commandId, 'action-fixture');

    const closed = stream.next();
    await hub.close('action-fixture', subscription());
    assert.deepEqual(await closed, { done: true, value: undefined });

    const reconnect = iterator(hub.subscribe(subscription({ signal: globalThis.AbortSignal.timeout(25) })));
    await hub.publish(command());
    assert.deepEqual(await reconnect.next(), { done: true, value: undefined });
});

test('SSE action hub closes a persistently replayed action that was never published in this process', async () => {
    const hub = new SseActionCommandHub();
    const stream = iterator(hub.subscribe(subscription()));
    const closed = stream.next();

    await hub.close('action-from-repository', subscription());

    assert.deepEqual(await closed, { done: true, value: undefined });
});

test('SSE action hub closes subscriptions on abort and expiry', async () => {
    const hub = new SseActionCommandHub();
    const controller = new globalThis.AbortController();
    const aborted = iterator(hub.subscribe(subscription({ signal: controller.signal })));
    const abortedNext = aborted.next();
    controller.abort();
    assert.deepEqual(await abortedNext, { done: true, value: undefined });

    const expired = iterator(hub.subscribe(subscription({ expiresAt: new Date(Date.now() + 20).toISOString() })));
    assert.deepEqual(await expired.next(), { done: true, value: undefined });

    const alreadyAborted = new globalThis.AbortController();
    alreadyAborted.abort();
    const closedBeforeSubscribe = iterator(hub.subscribe(subscription({ signal: alreadyAborted.signal })));
    assert.deepEqual(await closedBeforeSubscribe.next(), { done: true, value: undefined });
});

test('SSE action hub queues a command until the subscriber requests its next event', async () => {
    const hub = new SseActionCommandHub();
    const stream = iterator(hub.subscribe(subscription()));

    await hub.publish(command());

    assert.equal((await stream.next()).value.commandId, 'action-fixture');
    await stream.return();
});
