import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { test } from 'node:test';

import { streamReminderActions } from '../dist/infrastructure/http/gateway-sse-response.js';

function request(headers = {}) {
    const value = new EventEmitter();
    value.headers = { authorization: 'Bearer device-token', ...headers };
    return value;
}

function response(write = () => true) {
    const value = new EventEmitter();
    value.destroyed = false;
    value.writableNeedDrain = false;
    value.writableEnded = false;
    value.headers = undefined;
    value.chunks = [];
    value.writeHead = (_status, headers) => {
        value.headers = headers;
    };
    value.flushHeaders = () => {};
    value.write = (chunk) => {
        value.chunks.push(chunk);
        return write(chunk, value);
    };
    value.end = () => {
        value.writableEnded = true;
    };
    return value;
}

function event() {
    return {
        id: 'action-fixture',
        event: 'reminder.action',
        data: { correlationId: 'correlation-fixture', action: 'acknowledge' },
    };
}

function options(overrides = {}) {
    const events = [event()];
    return {
        request: request(),
        response: response(),
        url: new URL(
            'http://gateway/v1/devices/device-fixture/reminder-actions/stream?reminderType=strong&reminderTriggerId=trigger-fixture',
        ),
        runtime: {
            actionStreamApi: {
                connect: async () =>
                    (async function* stream() {
                        yield* events;
                    })(),
            },
        },
        logger: { log: () => {} },
        requestId: 'request-fixture',
        encodedDeviceId: 'device-fixture',
        correlationIdObserved: () => {},
        ...overrides,
    };
}

test('SSE response validates scope and encoded device IDs before creating a stream', async () => {
    await assert.rejects(
        () => streamReminderActions(options({ url: new URL('http://gateway/stream?reminderType=weak') })),
        /Invalid action stream scope/u,
    );
    await assert.rejects(
        () => streamReminderActions(options({ encodedDeviceId: '%ZZ' })),
        /Invalid encoded device id/u,
    );
});

test('SSE response serializes a command, preserves the replay cursor, and tolerates logging failures', async () => {
    const observed = [];
    const stream = options({
        request: request({ 'last-event-id': 'action-replay' }),
        logger: {
            log: () => {
                throw new Error('logging unavailable');
            },
        },
        correlationIdObserved: (correlationId) => observed.push(correlationId),
    });
    let input;
    stream.runtime.actionStreamApi.connect = async (value) => {
        input = value;
        return (async function* events() {
            yield event();
        })();
    };

    await streamReminderActions(stream);

    assert.deepEqual(input, {
        authorization: 'Bearer device-token',
        deviceId: 'device-fixture',
        reminderType: 'strong',
        reminderTriggerId: 'trigger-fixture',
        lastEventId: 'action-replay',
        signal: input.signal,
    });
    assert.match(stream.response.chunks.join(''), /id: action-fixture\nevent: reminder\.action/u);
    assert.deepEqual(observed, ['correlation-fixture']);
    assert.equal(stream.response.writableEnded, true);
});

test('SSE response stops after a closed backpressure wait or a destroyed response', async () => {
    const closed = response((_chunk, value) => {
        globalThis.queueMicrotask(() => value.emit('close'));
        return false;
    });
    await streamReminderActions(options({ response: closed }));
    assert.equal(closed.writableEnded, true);

    const destroyed = response();
    destroyed.destroyed = true;
    await streamReminderActions(options({ response: destroyed }));
    assert.equal(destroyed.chunks.length, 0);
    assert.equal(destroyed.writableEnded, true);
});
