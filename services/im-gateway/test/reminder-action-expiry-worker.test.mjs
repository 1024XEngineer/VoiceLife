import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ReminderActionExpiryWorker } from '../dist/infrastructure/reminder-action-expiry-worker.js';

function logger() {
    const entries = [];
    return { entries, log: (entry) => entries.push(entry) };
}

async function waitFor(predicate, message) {
    const deadline = Date.now() + 1_000;
    while (Date.now() < deadline) {
        if (predicate()) return;
        await new Promise((resolve) => globalThis.setTimeout(resolve, 2));
    }
    assert.fail(message);
}

test('runOnce expires due actions and emits a count only when work was done', async () => {
    const output = logger();
    let calls = 0;
    const worker = new ReminderActionExpiryWorker({
        actions: {
            expireDue: async () => {
                calls += 1;
                return calls === 1 ? 2 : 0;
            },
            recoverStaleProcessing: async () => 0,
        },
        logger: output,
    });

    assert.equal(await worker.runOnce(), 2);
    assert.equal(await worker.runOnce(), 0);
    assert.deepEqual(output.entries, [{ level: 'info', event: 'reminder.action.expiry.expired', count: 2 }]);
});

test('worker keeps polling after a transient expiry failure and closes cleanly', async () => {
    const output = logger();
    let expireCalls = 0;
    let recoverCalls = 0;
    const worker = new ReminderActionExpiryWorker({
        actions: {
            expireDue: async () => {
                expireCalls += 1;
                if (expireCalls === 1) throw new Error('temporary database outage');
                return expireCalls === 2 ? 1 : 0;
            },
            recoverStaleProcessing: async () => {
                recoverCalls += 1;
                return 0;
            },
        },
        logger: output,
        pollIntervalMs: 5,
    });

    worker.start();
    worker.start();
    await waitFor(() => expireCalls >= 2, 'expiry worker did not retry after the first failure');
    await worker.close();
    const callsAfterClose = expireCalls;
    await new Promise((resolve) => globalThis.setTimeout(resolve, 15));

    assert.equal(expireCalls, callsAfterClose);
    assert.ok(recoverCalls >= 2);
    assert.deepEqual(
        output.entries.map(({ level, event, count, errorCode }) => ({
            level,
            event,
            ...(count === undefined ? {} : { count }),
            ...(errorCode === undefined ? {} : { errorCode }),
        })),
        [
            { level: 'info', event: 'reminder.action.expiry.started' },
            { level: 'error', event: 'reminder.action.expiry.poll.failed', errorCode: 'Error' },
            { level: 'info', event: 'reminder.action.expiry.expired', count: 1 },
            { level: 'info', event: 'reminder.action.expiry.stopped' },
        ],
    );
});

test('worker rejects a non-positive polling interval', () => {
    assert.throws(
        () =>
            new ReminderActionExpiryWorker({
                actions: { expireDue: async () => 0, recoverStaleProcessing: async () => 0 },
                logger: logger(),
                pollIntervalMs: 0,
            }),
        /positive integer/u,
    );
});
