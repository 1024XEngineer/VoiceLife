import { readFile } from 'node:fs/promises';
import { isDeepStrictEqual } from 'node:util';

const FIXTURE_ROOT = new URL('../../../contracts/im-gateway/v1/fixtures/', import.meta.url);
const T0 = '2099-08-18T00:00:00.000Z';

export const QUICK_SCENARIOS = [
    'gateway-restart',
    'worker-send-restart',
    'retry-backoff',
    'dead-letter',
    'sse-recovery',
];

export const FULL_SCENARIOS = [
    'gateway-restart',
    'worker-claim-restart',
    'worker-send-restart',
    'retry-backoff',
    'dead-letter',
    'retry-exhaustion',
    'sse-recovery',
    'sse-race-backpressure',
];

export const SCENARIOS = {
    'gateway-restart': gatewayRestart,
    'worker-claim-restart': workerClaimRestart,
    'worker-send-restart': workerSendRestart,
    'retry-backoff': retryBackoff,
    'dead-letter': deadLetter,
    'retry-exhaustion': retryExhaustion,
    'sse-recovery': sseRecovery,
    'sse-race-backpressure': sseRaceBackpressure,
};

async function gatewayRestart(context) {
    let gateway = await context.startGateway(T0);
    const intent = await strongIntent(context, gateway.ready, 'gateway_restart');
    const first = await postNotification(gateway.ready.origin, intent, gateway.ready.target.token);
    assert(first.status === 202, 'notification_not_committed');
    const firstBody = await first.json();
    let snapshot = await context.snapshot(gateway.child, intent.businessEventId);
    assert(snapshot.deliveries[0]?.status === 'pending', 'delivery_not_pending_before_restart');
    assert(snapshot.attempts.length === 0 && snapshot.platformSends.length === 0, 'dispatch_started_before_restart');

    await context.stop(gateway.child);
    gateway = await context.startGateway(T0);
    const replay = await postNotification(gateway.ready.origin, intent, gateway.ready.target.token);
    assert(replay.status === 202 && isDeepStrictEqual(await replay.json(), firstBody), 'restart_changed_submission');
    const worker = await context.startWorker('success', T0);
    assert((await context.request(worker.child, 'run_once')).claimed === 1, 'recovered_event_not_claimed');
    snapshot = await context.snapshot(worker.child, intent.businessEventId);
    assertAcceptedOnce(snapshot);
    return result(6, snapshot, 'notification_committed');
}

async function workerClaimRestart(context) {
    const gateway = await context.startGateway(T0);
    const intent = await submitStrong(context, gateway, 'worker_claim_restart');
    let worker = await context.startWorker('success', T0);
    assert((await context.request(worker.child, 'claim_only')).claimed === 1, 'outbox_not_claimed');
    await context.kill(worker.child);
    worker = await context.startWorker('success', plusMinutes(T0, 3));
    assert((await context.request(worker.child, 'run_once')).claimed === 1, 'leased_event_not_reclaimed');
    const snapshot = await context.snapshot(worker.child, intent.businessEventId);
    assertAcceptedOnce(snapshot);
    return result(4, snapshot, 'after_outbox_claim');
}

async function workerSendRestart(context) {
    const gateway = await context.startGateway(T0);
    const intent = await submitStrong(context, gateway, 'worker_send_restart');
    let worker = await context.startWorker('pause-after-send', T0);
    const observed = context.waitForInjection(worker.child, 'after_platform_send');
    const interruptedRun = context.request(worker.child, 'run_once').catch(() => undefined);
    await observed;
    let snapshot = await context.snapshot(gateway.child, intent.businessEventId);
    assert(snapshot.deliveries[0]?.status === 'sending', 'delivery_not_in_flight_at_crash');
    assert(snapshot.platformSends.length === 1, 'platform_side_effect_missing');
    await context.kill(worker.child);
    await interruptedRun;

    worker = await context.startWorker('success', plusMinutes(T0, 3));
    assert((await context.request(worker.child, 'run_once')).claimed === 1, 'crashed_dispatch_not_reclaimed');
    snapshot = await context.snapshot(worker.child, intent.businessEventId);
    assert(snapshot.deliveries[0]?.status === 'accepted', 'recovered_dispatch_not_accepted');
    assert(snapshot.platformSends.length === 1, 'platform_send_duplicated');
    assert(snapshot.platformCalls.length === 2, 'idempotent_platform_replay_not_observed');
    return result(6, snapshot, 'after_platform_send');
}

async function retryBackoff(context) {
    const gateway = await context.startGateway(T0);
    const intent = await submitStrong(context, gateway, 'retry_backoff');
    const worker = await context.startWorker('retry-once', T0);
    assert((await context.request(worker.child, 'run_once')).claimed === 1, 'first_attempt_not_claimed');
    let snapshot = await context.snapshot(worker.child, intent.businessEventId);
    assert(snapshot.deliveries[0]?.status === 'retryable_failed', 'temporary_failure_not_retryable');
    const retry = snapshot.outbox.find((event) => event.event_type === 'im.delivery.retry-scheduled');
    assert(retry?.available_at === plusMinutes(T0, 1), 'fixed_clock_backoff_mismatch');
    assert((await context.request(worker.child, 'run_once')).claimed === 0, 'retry_ran_before_next_attempt');
    const advanced = await context.request(worker.child, 'advance', { minutes: 1 });
    assert(advanced.now === plusMinutes(T0, 1), 'fixed_clock_not_advanced');
    assert((await context.request(worker.child, 'run_once')).claimed === 1, 'due_retry_not_claimed');
    snapshot = await context.snapshot(worker.child, intent.businessEventId);
    assert(snapshot.deliveries[0]?.status === 'accepted', 'retry_did_not_recover');
    assert(snapshot.attempts.length === 2 && snapshot.platformSends.length === 1, 'retry_counts_mismatch');
    return result(8, snapshot, 'temporary_platform_failure');
}

async function deadLetter(context) {
    const gateway = await context.startGateway(T0);
    const intent = await submitStrong(context, gateway, 'dead_letter');
    const worker = await context.startWorker('permanent', T0);
    assert((await context.request(worker.child, 'run_once')).claimed === 1, 'permanent_attempt_not_claimed');
    const snapshot = await context.snapshot(worker.child, intent.businessEventId);
    assert(snapshot.deliveries[0]?.status === 'dead_letter', 'permanent_failure_not_dead_lettered');
    assert(snapshot.deliveries[0]?.last_error_code === 'e2e_platform_permanent', 'dead_letter_reason_mismatch');
    assert(
        snapshot.platformSends.length === 0 && snapshot.platformCalls.length === 1,
        'permanent_send_counts_mismatch',
    );
    return result(4, snapshot, 'permanent_platform_failure');
}

async function retryExhaustion(context) {
    const gateway = await context.startGateway(T0);
    const intent = await submitWeak(context, gateway, 'retry_exhaustion');
    const worker = await context.startWorker('retry-always', T0);
    const delays = [1, 2, 4, 8];
    for (let attempt = 0; attempt < 5; attempt += 1) {
        assert((await context.request(worker.child, 'run_once')).claimed === 1, 'retry_attempt_not_claimed');
        if (attempt < delays.length) await context.request(worker.child, 'advance', { minutes: delays[attempt] });
    }
    const snapshot = await context.snapshot(worker.child, intent.businessEventId);
    assert(snapshot.deliveries[0]?.status === 'dead_letter', 'retry_limit_not_dead_lettered');
    assert(snapshot.deliveries[0]?.last_error_code === 'delivery_retry_exhausted', 'retry_limit_reason_mismatch');
    assert(snapshot.attempts.length === 5 && snapshot.platformCalls.length === 5, 'retry_limit_counts_mismatch');
    return result(8, snapshot, 'retry_limit_reached');
}

async function sseRecovery(context) {
    let gateway = await context.startGateway(T0);
    const intent = await submitStrong(context, gateway, 'sse_recovery');
    const worker = await context.startWorker('success', T0);
    await context.request(worker.child, 'run_once');
    let snapshot = await context.snapshot(worker.child, intent.businessEventId);
    const actionToken = snapshot.platformSends[0]?.action_token;
    assert(typeof actionToken === 'string', 'action_token_missing');

    const stream = await openStream(gateway.ready.origin, gateway.ready.target, intent.reminderTriggerId);
    assert((await stream.nextFrame()).comment === 'heartbeat', 'sse_heartbeat_missing');
    const submitted = await submitAction(gateway.ready.origin, actionToken);
    assert(submitted.status === 200, 'action_submission_failed');
    await submitted.text();
    const event = await stream.nextEvent();
    assert(event.data.correlationId === intent.correlationId, 'action_correlation_mismatch');
    await stream.cancel();

    await context.kill(gateway.child);
    gateway = await context.startGateway(T0);
    const replay = await openStream(gateway.ready.origin, gateway.ready.target, intent.reminderTriggerId, event.id);
    const replayed = await replay.nextEvent();
    assert(replayed.id === event.id, 'last_event_id_replay_missing');
    await replay.cancel();
    const isolated = await globalThis.fetch(
        streamUrl(gateway.ready.origin, gateway.ready.isolation, intent.reminderTriggerId),
        {
            headers: { authorization: `Bearer ${gateway.ready.isolation.token}` },
        },
    );
    assert(isolated.status === 410, 'sse_device_isolation_failed');
    await isolated.text();

    const resultBody = await actionResult(event.data, intent.reminderTriggerId);
    const path = actionResultUrl(gateway.ready.origin, gateway.ready.target.deviceId, event.id);
    const first = await postJson(path, resultBody, gateway.ready.target.token);
    const duplicate = await postJson(
        path,
        { ...resultBody, occurredAt: plusSeconds(resultBody.occurredAt, 1) },
        gateway.ready.target.token,
    );
    assert(first.status === 200 && duplicate.status === 200, 'action_result_not_accepted');
    assert(isDeepStrictEqual(await first.json(), await duplicate.json()), 'duplicate_result_changed_action');
    snapshot = await context.snapshot(gateway.child, intent.businessEventId);
    assert(snapshot.actions[0]?.status === 'succeeded', 'action_result_not_persisted');
    return result(10, snapshot, 'sse_disconnect');
}

async function sseRaceBackpressure(context) {
    const gateway = await context.startGateway(T0);
    const intent = await submitStrong(context, gateway, 'sse_race');
    const worker = await context.startWorker('success', T0);
    await context.request(worker.child, 'run_once');
    let snapshot = await context.snapshot(worker.child, intent.businessEventId);
    const actionToken = snapshot.platformSends[0]?.action_token;
    assert(typeof actionToken === 'string', 'race_action_token_missing');

    await context.request(gateway.child, 'arm_replay');
    const barrier = context.waitForInjection(gateway.child, 'replay_after_subscribe');
    const streamPromise = openStream(gateway.ready.origin, gateway.ready.target, intent.reminderTriggerId);
    await barrier;
    const submitted = await submitAction(gateway.ready.origin, actionToken);
    assert(submitted.status === 200, 'race_action_submission_failed');
    await submitted.text();
    await context.request(gateway.child, 'release_replay');
    const stream = await streamPromise;
    const event = await stream.nextEvent();
    assert(event.data.correlationId === intent.correlationId, 'race_event_lost');
    assert(await stream.noEvent(100), 'race_event_duplicated');
    await stream.cancel();
    const pressure = await context.request(gateway.child, 'backpressure');
    assert(pressure.closed === true && pressure.overflowCount === 1, 'slow_consumer_not_bounded');
    snapshot = await context.snapshot(gateway.child, intent.businessEventId);
    assert(snapshot.actions.length === 1, 'race_created_duplicate_action');
    return result(6, snapshot, 'publish_replay_race');
}

async function submitStrong(context, gateway, suffix) {
    const intent = await strongIntent(context, gateway.ready, suffix);
    const response = await postNotification(gateway.ready.origin, intent, gateway.ready.target.token);
    assert(response.status === 202, 'notification_not_accepted');
    await response.json();
    return intent;
}

async function submitWeak(context, gateway, suffix) {
    const intent = await notificationIntent(context, gateway.ready, suffix, 'notification-weak.json');
    const response = await postNotification(gateway.ready.origin, intent, gateway.ready.target.token);
    assert(response.status === 202, 'notification_not_accepted');
    await response.json();
    return intent;
}

async function strongIntent(context, ready, suffix) {
    return notificationIntent(context, ready, suffix, 'notification-strong.json');
}

async function notificationIntent(context, ready, suffix, fixtureName) {
    const fixture = JSON.parse(await readFile(new URL(fixtureName, FIXTURE_ROOT), 'utf8'));
    const eventId = `${context.prefix}_${suffix}`;
    return {
        ...fixture,
        businessEventId: eventId,
        correlationId: `${eventId}_correlation`,
        recipient: { userId: ready.target.userId, deviceId: ready.target.deviceId },
        scheduleId: `${eventId}_schedule`,
        taskId: `${eventId}_task`,
        instanceId: `${eventId}_instance`,
        reminderTriggerId: `${eventId}_trigger`,
        plannedAt: T0,
        triggerAt: T0,
        occurredAt: T0,
    };
}

function postNotification(origin, body, token) {
    return postJson(`${origin}/v1/im/notifications`, body, token, { 'idempotency-key': body.businessEventId });
}

function submitAction(origin, token) {
    return globalThis.fetch(`${origin}/voicelife/reminder-actions/${encodeURIComponent(token)}`, {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded' },
        body: new globalThis.URLSearchParams({ action: 'acknowledge' }),
    });
}

async function actionResult(command, reminderTriggerId) {
    const fixture = JSON.parse(await readFile(new URL('reminder-action-result.json', FIXTURE_ROOT), 'utf8'));
    const body = { ...fixture, operationId: command.operationId, reminderTriggerId, occurredAt: T0 };
    delete body.nextTriggerAt;
    return body;
}

function postJson(url, body, token, extra = {}) {
    return globalThis.fetch(url, {
        method: 'POST',
        headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json', ...extra },
        body: JSON.stringify(body),
    });
}

function streamUrl(origin, device, triggerId) {
    return `${origin}/v1/devices/${encodeURIComponent(device.deviceId)}/reminder-actions/stream?reminderType=strong&reminderTriggerId=${encodeURIComponent(triggerId)}`;
}

async function openStream(origin, device, triggerId, lastEventId) {
    const response = await globalThis.fetch(streamUrl(origin, device, triggerId), {
        headers: {
            authorization: `Bearer ${device.token}`,
            ...(lastEventId === undefined ? {} : { 'last-event-id': lastEventId }),
        },
    });
    assert(response.status === 200 && response.body !== null, 'sse_connection_failed');
    const reader = response.body.getReader();
    let buffer = '';
    const nextFrame = async (timeoutMs = 2000) => {
        const deadline = Date.now() + timeoutMs;
        while (true) {
            const boundary = buffer.indexOf('\n\n');
            if (boundary >= 0) {
                const raw = buffer.slice(0, boundary);
                buffer = buffer.slice(boundary + 2);
                if (raw.startsWith(': ')) return { comment: raw.slice(2) };
                const id = /^id: (.+)$/mu.exec(raw)?.[1];
                const data = /^data: (.+)$/mu.exec(raw)?.[1];
                if (id !== undefined && data !== undefined) return { id, data: JSON.parse(data) };
            }
            const remaining = deadline - Date.now();
            if (remaining <= 0) throw new Error('sse_frame_timeout');
            const chunk = await Promise.race([
                reader.read(),
                new Promise((_, reject) =>
                    globalThis.setTimeout(() => reject(new Error('sse_frame_timeout')), remaining),
                ),
            ]);
            if (chunk.done) throw new Error('sse_stream_ended');
            buffer += new globalThis.TextDecoder().decode(chunk.value);
        }
    };
    return {
        nextFrame,
        async nextEvent(timeoutMs) {
            while (true) {
                const frame = await nextFrame(timeoutMs);
                if (frame.id !== undefined) return frame;
            }
        },
        async noEvent(timeoutMs) {
            const deadline = Date.now() + timeoutMs;
            while (true) {
                try {
                    const frame = await nextFrame(Math.max(1, deadline - Date.now()));
                    if (frame.id !== undefined) return false;
                    if (Date.now() >= deadline) return true;
                } catch (error) {
                    return error instanceof Error && error.message === 'sse_frame_timeout';
                }
            }
        },
        cancel: () => reader.cancel(),
    };
}

function actionResultUrl(origin, deviceId, commandId) {
    return `${origin}/v1/devices/${encodeURIComponent(deviceId)}/reminder-actions/${encodeURIComponent(commandId)}/result`;
}

function assertAcceptedOnce(snapshot) {
    assert(snapshot.deliveries.length === 1 && snapshot.deliveries[0].status === 'accepted', 'delivery_not_accepted');
    assert(snapshot.attempts.length === 1, 'delivery_attempt_count_mismatch');
    assert(snapshot.platformSends.length === 1, 'platform_send_count_mismatch');
}

function result(assertions, snapshot, injectionPoint) {
    return { assertions, injectionPoint, state: evidenceState(snapshot) };
}

export function evidenceState(snapshot) {
    return {
        deliveries: snapshot.deliveries.map((item) => ({
            deliveryId: item.id,
            businessEventId: item.business_event_id,
            correlationId: item.correlation_id,
            status: item.status,
            lastErrorCode: item.last_error_code ?? null,
        })),
        attempts: snapshot.attempts.map((item) => ({
            attemptId: item.id,
            deliveryId: item.delivery_id,
            status: item.status,
        })),
        outbox: snapshot.outbox.map((item) => ({
            outboxEventId: item.id,
            deliveryId: item.aggregate_id,
            status: item.status,
        })),
        actions: snapshot.actions.map((item) => ({
            commandId: item.id,
            operationId: item.operation_id,
            correlationId: item.correlation_id,
            deliveryId: item.delivery_id,
            status: item.status,
        })),
        platformCallCount: snapshot.platformCalls.length,
        platformSendCount: snapshot.platformSends.length,
    };
}

function plusMinutes(value, minutes) {
    return new Date(Date.parse(value) + minutes * 60_000).toISOString();
}

function plusSeconds(value, seconds) {
    return new Date(Date.parse(value) + seconds * 1000).toISOString();
}

function assert(condition, code) {
    if (!condition) throw new RecoveryAssertionError(code);
}

export class RecoveryAssertionError extends Error {
    constructor(code) {
        super(code);
        this.code = code;
    }
}
