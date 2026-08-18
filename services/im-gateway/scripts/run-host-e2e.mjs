import { randomUUID } from 'node:crypto';
import { fork } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { isDeepStrictEqual } from 'node:util';
import { Client } from 'pg';

const GATEWAY_SCRIPT = fileURLToPath(new URL('./start-host-e2e-gateway.mjs', import.meta.url));
const FIXTURE_ROOT = new URL('../../../contracts/im-gateway/v1/fixtures/', import.meta.url);
const IPC_TIMEOUT_MS = 5000;

/** Run the smallest production-boundary journey required by issue #285. */
export async function runHostE2e({
    databaseUrl = process.env.DATABASE_URL,
    runId = randomUUID().replaceAll('-', ''),
} = {}) {
    if (typeof databaseUrl !== 'string' || databaseUrl.trim() === '') throw new Error('database_required');
    if (!/^[0-9a-f]{32}$/u.test(runId)) throw new Error('run_id_invalid');
    const prefix = `e2e_${runId}`;
    const admin = new Client({ connectionString: databaseUrl });
    const streams = new Set();
    let adminConnected = false;
    let schemaCreated = false;
    let child;
    let result;
    let primaryError;

    try {
        await admin.connect();
        adminConnected = true;
        await admin.query(`CREATE SCHEMA "${prefix}"`);
        schemaCreated = true;
        const scopedDatabaseUrl = new URL(databaseUrl);
        scopedDatabaseUrl.searchParams.set('options', `-c search_path=${prefix}`);
        child = fork(GATEWAY_SCRIPT, [], {
            env: {
                E2E_DATABASE_URL: scopedDatabaseUrl.toString(),
                E2E_PREFIX: prefix,
                NODE_ENV: 'test',
            },
            stdio: ['ignore', 'pipe', 'pipe', 'ipc'],
        });
        child.stdout?.resume();
        child.stderr?.resume();
        const ready = await waitForReady(child);
        result = await runJourney(child, ready, prefix, streams);
    } catch (error) {
        primaryError = error;
    } finally {
        const cleanupErrors = [];
        for (const stream of streams) await cleanupStep(cleanupErrors, () => stream.cancel());
        if (child !== undefined) await cleanupStep(cleanupErrors, () => stopChild(child));
        if (schemaCreated) {
            await cleanupStep(cleanupErrors, () => admin.query(`DROP SCHEMA "${prefix}" CASCADE`));
        }
        if (adminConnected) await cleanupStep(cleanupErrors, () => admin.end());
        if (cleanupErrors.length > 0) primaryError = new HostE2eError('cleanup_failed');
    }

    if (primaryError !== undefined) throw primaryError;
    return result;
}

async function runJourney(child, ready, prefix, streams) {
    const [strongFixture, weakFixture, resultFixture] = await Promise.all([
        readFixture('notification-strong.json'),
        readFixture('notification-weak.json'),
        readFixture('reminder-action-result.json'),
    ]);
    const timestamp = new Date().toISOString();
    const triggerId = `${prefix}_trigger`;
    const strong = notification(strongFixture, {
        eventId: `${prefix}_strong`,
        triggerId,
        timestamp,
        ...ready.target,
    });
    const isolationStrong = notification(strongFixture, {
        eventId: `${prefix}_isolation_strong`,
        triggerId,
        timestamp,
        ...ready.isolation,
    });
    const weak = notification(weakFixture, {
        eventId: `${prefix}_weak`,
        triggerId: `${prefix}_weak_trigger`,
        timestamp,
        ...ready.target,
    });
    const observedResponses = [];
    let assertions = 0;

    const health = await globalThis.fetch(`${ready.origin}/healthz`);
    assert(health.status === 200, 'gateway_health_check_failed');
    assertions += 1;

    const [first, identical] = await Promise.all([
        postNotification(ready.origin, strong, ready.target.token),
        postNotification(ready.origin, strong, ready.target.token),
    ]);
    assert(first.status === 202 && identical.status === 202, 'notification_not_accepted');
    const firstBody = await first.json();
    const identicalBody = await identical.json();
    observedResponses.push(firstBody, identicalBody);
    assert(isDeepStrictEqual(firstBody, identicalBody), 'identical_replay_changed_response');
    assertions += 2;

    let snapshot = await waitForSnapshot(child, (value) => value.sendCount === 1);
    assert(
        snapshot.dispatchRaces.some(
            (race) => race.length === 2 && race.reduce((total, claims) => total + claims, 0) === 1,
        ),
        'concurrent_dispatch_not_observed',
    );
    assert(snapshot.targetStatus === 'accepted' && snapshot.receiptCount === 0, 'acceptance_stage_not_isolated');
    assertions += 2;

    await request(child, 'deliver');
    snapshot = await waitForSnapshot(child, (value) => value.targetStatus === 'delivered');
    assert(snapshot.receiptCount === 1, 'async_receipt_not_recorded');
    assertions += 1;

    const conflict = await postNotification(
        ready.origin,
        { ...strong, content: { ...strong.content, title: 'Conflicting fixture reminder' } },
        ready.target.token,
    );
    assert(conflict.status === 409, 'conflicting_replay_not_rejected');
    assertions += 1;

    const isolationResponse = await postNotification(ready.origin, isolationStrong, ready.isolation.token);
    const weakResponse = await postNotification(ready.origin, weak, ready.target.token);
    assert(isolationResponse.status === 202 && weakResponse.status === 202, 'routing_notification_not_accepted');
    observedResponses.push(await isolationResponse.json(), await weakResponse.json());
    snapshot = await waitForSnapshot(child, (value) => value.sendCount === 3);
    assert(snapshot.weakHasActionToken === false, 'weak_reminder_created_action_ui');
    assert(
        typeof snapshot.targetActionToken === 'string' && typeof snapshot.isolationActionToken === 'string',
        'strong_reminder_missing_action_token',
    );
    assertions += 3;

    const targetStream = await openStream(ready.origin, ready.target.deviceId, triggerId, ready.target.token);
    streams.add(targetStream);
    const isolationStream = await openStream(ready.origin, ready.isolation.deviceId, triggerId, ready.isolation.token);
    streams.add(isolationStream);

    const targetActionPage = await submitAction(ready.origin, snapshot.targetActionToken);
    assert(targetActionPage.status === 200, 'target_action_submission_failed');
    observedResponses.push(await targetActionPage.text());
    const event = await targetStream.next();
    assert(event?.data?.commandId && event.data.deviceId === ready.target.deviceId, 'target_sse_command_missing');
    await targetStream.cancel();
    streams.delete(targetStream);
    assertions += 2;

    const replayStream = await openStream(ready.origin, ready.target.deviceId, triggerId, ready.target.token, event.id);
    streams.add(replayStream);
    const replay = await replayStream.next();
    assert(replay?.id === event.id, 'last_event_id_replay_failed');
    await replayStream.cancel();
    streams.delete(replayStream);
    assertions += 1;

    const isolationActionPage = await submitAction(ready.origin, snapshot.isolationActionToken);
    assert(isolationActionPage.status === 200, 'isolation_action_submission_failed');
    observedResponses.push(await isolationActionPage.text());
    const isolatedEvent = await isolationStream.next();
    assert(
        isolatedEvent?.data?.deviceId === ready.isolation.deviceId && isolatedEvent.id !== event.id,
        'sse_device_isolation_failed',
    );
    await isolationStream.cancel();
    streams.delete(isolationStream);
    assertions += 2;

    const firstResultBody = actionResult(resultFixture, event.data.operationId, triggerId, timestamp);
    const replayResultBody = {
        ...firstResultBody,
        occurredAt: new Date(Date.parse(firstResultBody.occurredAt) + 1000).toISOString(),
    };
    const resultPath = `${ready.origin}/v1/devices/${encodeURIComponent(ready.target.deviceId)}/reminder-actions/${encodeURIComponent(event.id)}/result`;
    const firstResult = await postJson(resultPath, firstResultBody, ready.target.token);
    assert(firstResult.status === 200, 'action_result_not_accepted');
    const firstResultResponse = await firstResult.json();
    observedResponses.push(firstResultResponse);
    const firstAction = targetAction(await request(child, 'snapshot'), ready.target.deviceId);
    const duplicateResult = await postJson(resultPath, replayResultBody, ready.target.token);
    assert(duplicateResult.status === 200, 'action_result_replay_not_accepted');
    const duplicateResultResponse = await duplicateResult.json();
    observedResponses.push(duplicateResultResponse);
    const duplicateAction = targetAction(await request(child, 'snapshot'), ready.target.deviceId);
    assert(isDeepStrictEqual(firstResultResponse, duplicateResultResponse), 'action_result_response_changed');
    assert(isDeepStrictEqual(firstAction, duplicateAction), 'action_result_persistence_changed');
    assert(firstAction.result?.occurredAt === firstResultBody.occurredAt, 'action_result_original_not_preserved');
    assertions += 5;

    snapshot = await request(child, 'snapshot');
    assert(
        snapshot.deliveryCount === 3 &&
            snapshot.attemptCount === 3 &&
            snapshot.receiptCount === 1 &&
            snapshot.actionCount === 2 &&
            snapshot.sendCount === 3,
        'persistence_counts_mismatch',
    );
    const serializedResponses = JSON.stringify(observedResponses);
    assert(
        snapshot.sensitiveLogLeak === false &&
            !serializedResponses.includes(ready.target.token) &&
            !serializedResponses.includes(ready.isolation.token),
        'sensitive_evidence_leaked',
    );
    assertions += 2;

    return {
        assertions,
        deliveryCount: snapshot.deliveryCount,
        sendCount: snapshot.sendCount,
        receiptCount: snapshot.receiptCount,
        actionCount: snapshot.actionCount,
        workerCount: 2,
    };
}

function notification(fixture, { eventId, triggerId, timestamp, deviceId, userId }) {
    return {
        ...fixture,
        businessEventId: eventId,
        correlationId: `${eventId}_correlation`,
        recipient: { userId, deviceId },
        scheduleId: `${eventId}_schedule`,
        taskId: `${eventId}_task`,
        instanceId: `${eventId}_instance`,
        reminderTriggerId: triggerId,
        plannedAt: timestamp,
        triggerAt: timestamp,
        occurredAt: timestamp,
    };
}

function actionResult(fixture, operationId, reminderTriggerId, occurredAt) {
    const result = { ...fixture, operationId, reminderTriggerId, occurredAt };
    delete result.nextTriggerAt;
    return result;
}

function targetAction(snapshot, deviceId) {
    const action = snapshot.actions.find((candidate) => candidate.deviceId === deviceId);
    assert(action !== undefined, 'persisted_action_missing');
    return action;
}

async function readFixture(name) {
    return JSON.parse(await readFile(new URL(name, FIXTURE_ROOT), 'utf8'));
}

async function postNotification(origin, body, token) {
    return postJson(`${origin}/v1/im/notifications`, body, token, { 'idempotency-key': body.businessEventId });
}

async function postJson(url, body, token, extra = {}) {
    return globalThis.fetch(url, {
        method: 'POST',
        headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json', ...extra },
        body: JSON.stringify(body),
    });
}

function submitAction(origin, token) {
    return globalThis.fetch(`${origin}/voicelife/reminder-actions/${encodeURIComponent(token)}`, {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded' },
        body: new globalThis.URLSearchParams({ action: 'acknowledge' }),
    });
}

async function openStream(origin, deviceId, triggerId, token, lastEventId) {
    const url = `${origin}/v1/devices/${encodeURIComponent(deviceId)}/reminder-actions/stream?reminderType=strong&reminderTriggerId=${encodeURIComponent(triggerId)}`;
    const response = await globalThis.fetch(url, {
        headers: {
            authorization: `Bearer ${token}`,
            ...(lastEventId === undefined ? {} : { 'last-event-id': lastEventId }),
        },
    });
    assert(response.status === 200 && response.body !== null, 'sse_connection_failed');
    const reader = response.body.getReader();
    let buffer = '';
    return {
        async next() {
            while (true) {
                const boundary = buffer.indexOf('\n\n');
                if (boundary >= 0) {
                    const frame = buffer.slice(0, boundary);
                    buffer = buffer.slice(boundary + 2);
                    const id = /^id: (.+)$/m.exec(frame)?.[1];
                    const data = /^data: (.+)$/m.exec(frame)?.[1];
                    if (id !== undefined && data !== undefined) return { id, data: JSON.parse(data) };
                }
                const chunk = await reader.read();
                if (chunk.done) throw new Error('sse_ended_before_event');
                buffer += new globalThis.TextDecoder().decode(chunk.value);
            }
        },
        cancel: () => reader.cancel(),
    };
}

async function waitForReady(child) {
    const message = await waitForMessage(
        child,
        (candidate) => candidate.type === 'ready' || candidate.type === 'failed',
    );
    if (message.type === 'failed') throw new HostE2eError(message.code);
    return message;
}

async function request(child, type) {
    const requestId = request.nextId;
    request.nextId += 1;
    const response = waitForMessage(
        child,
        (candidate) => candidate.type === 'response' && candidate.requestId === requestId,
    );
    child.send({ type, requestId });
    const message = await response;
    if (typeof message.error === 'string') throw new HostE2eError(message.error);
    return message.data;
}
request.nextId = 1;

function waitForMessage(child, predicate, timeoutMs = IPC_TIMEOUT_MS) {
    return new Promise((resolve, reject) => {
        const finish = (error, value) => {
            globalThis.clearTimeout(timer);
            child.off('message', onMessage);
            child.off('exit', onExit);
            if (error === undefined) resolve(value);
            else reject(error);
        };
        const onMessage = (message) => {
            if (typeof message === 'object' && message !== null && predicate(message)) finish(undefined, message);
        };
        const onExit = () => finish(new HostE2eError('gateway_process_exited'));
        const timer = globalThis.setTimeout(() => finish(new HostE2eError('gateway_process_timeout')), timeoutMs);
        child.on('message', onMessage);
        child.once('exit', onExit);
    });
}

async function waitForSnapshot(child, predicate, timeoutMs = IPC_TIMEOUT_MS) {
    const deadline = Date.now() + timeoutMs;
    while (true) {
        const snapshot = await request(child, 'snapshot');
        if (predicate(snapshot)) return snapshot;
        if (Date.now() >= deadline) throw new HostE2eError('journey_timeout');
        await new Promise((resolve) => globalThis.setTimeout(resolve, 10));
    }
}

async function stopChild(child) {
    if (child.exitCode !== null || child.signalCode !== null) return;
    let stopError;
    try {
        await request(child, 'stop');
    } catch (error) {
        stopError = error;
    }
    if (child.exitCode === null && child.signalCode === null) {
        if (stopError !== undefined) child.kill('SIGTERM');
        try {
            await waitForExit(child);
        } catch {
            stopError = new HostE2eError('gateway_exit_timeout');
            child.kill('SIGKILL');
            await waitForExit(child);
        }
    }
    if (stopError !== undefined) throw stopError;
    if (child.exitCode !== 0) throw new HostE2eError('gateway_process_cleanup_failed');
}

function waitForExit(child, timeoutMs = IPC_TIMEOUT_MS) {
    if (child.exitCode !== null || child.signalCode !== null) return Promise.resolve();
    return new Promise((resolve, reject) => {
        const finish = (error) => {
            globalThis.clearTimeout(timer);
            child.off('exit', onExit);
            if (error === undefined) resolve();
            else reject(error);
        };
        const onExit = () => finish();
        const timer = globalThis.setTimeout(() => finish(new HostE2eError('gateway_exit_timeout')), timeoutMs);
        child.once('exit', onExit);
    });
}

async function cleanupStep(errors, callback) {
    try {
        await callback();
    } catch (error) {
        errors.push(error);
    }
}

function assert(condition, code) {
    if (!condition) throw new HostE2eError(code);
}

class HostE2eError extends Error {
    constructor(code) {
        super(code);
        this.code = code;
    }
}

function infrastructureFailure(error) {
    return (
        error !== null &&
        typeof error === 'object' &&
        'code' in error &&
        ['ECONNREFUSED', 'ENOTFOUND', 'ETIMEDOUT', 'infrastructure_failed'].includes(error.code)
    );
}

if (import.meta.url === `file://${process.argv[1]}`) {
    runHostE2e({ runId: process.env.E2E_RUN_ID })
        .then((journeyResult) => process.stdout.write(`${JSON.stringify(journeyResult)}\n`))
        .catch((error) => {
            const code =
                error instanceof HostE2eError && error.code === 'cleanup_failed'
                    ? 'host_e2e_cleanup_failed'
                    : infrastructureFailure(error)
                      ? 'host_e2e_infrastructure_failed'
                      : 'host_e2e_failed';
            process.stderr.write(`${code}\n`);
            process.exitCode = 1;
        });
}
