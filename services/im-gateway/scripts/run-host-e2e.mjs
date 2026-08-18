import { createHash, randomUUID } from 'node:crypto';
import { isDeepStrictEqual } from 'node:util';
import { Client } from 'pg';

import { createImGateway } from '../dist/app/create-im-gateway.js';
import { startGatewayHttpServer } from '../dist/infrastructure/http/gateway-http-server.js';
import { DeliveryOutboxWorker } from '../dist/infrastructure/delivery-outbox-worker.js';
import { DirectConversationResolver, SystemClock } from '../dist/infrastructure/production-support.js';
import {
    InMemoryActionTokenPort,
    MockChannelCapabilities,
    MockDeliveryRenderer,
    MockPairingCodePort,
} from '../dist/infrastructure/mock-support.js';
import { SseActionCommandHub } from '../dist/infrastructure/sse/sse-action-command-hub.js';
import { DatabaseDeviceAuthenticationPort, UuidIdGenerator } from '../dist/infrastructure/security/production-ports.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';

const DEVICE_TOKEN = 'host-e2e-token-1234567890123456789012345678';
const SECRET = 'host-e2e-external-identity-ciphertext';

/** Run the smallest production-boundary journey required by issue #285. */
export async function runHostE2e({
    databaseUrl = process.env.DATABASE_URL,
    runId = randomUUID().replaceAll('-', ''),
} = {}) {
    if (typeof databaseUrl !== 'string' || databaseUrl.trim() === '') throw new Error('database_required');
    if (!/^[0-9a-f]{32}$/u.test(runId)) throw new Error('run_id_invalid');
    const prefix = `e2e_${runId.slice(0, 24)}`;
    const deviceId = `${prefix}_device`;
    const userId = `${prefix}_user`;
    const channelId = `${prefix}_channel`;
    const identityId = `${prefix}_identity`;
    const bindingId = `${prefix}_binding`;
    const tokenDigest = createHash('sha256').update(DEVICE_TOKEN, 'utf8').digest();
    const now = new Date().toISOString();
    const logs = [];
    const sends = [];
    const clock = new SystemClock();
    const admin = new Client({ connectionString: databaseUrl });
    let uow;
    let schemaCreated = false;
    let server;
    let worker;
    try {
        await admin.connect();
        await admin.query(`CREATE SCHEMA "${prefix}"`);
        schemaCreated = true;
        const scopedDatabaseUrl = new URL(databaseUrl);
        scopedDatabaseUrl.searchParams.set('options', `-c search_path=${prefix}`);
        uow = new PostgresImUnitOfWork(scopedDatabaseUrl.toString());
        await uow.migrate();
        await uow.transaction(async (tx) => {
            await tx.devices.create({
                deviceId,
                userId,
                tokenDigest,
                status: 'active',
                createdAt: now,
                updatedAt: now,
            });
            await tx.channelAccounts.save({
                id: channelId,
                platform: 'wechat_official',
                tenantExternalId: `${prefix}_tenant`,
                koishiBotId: `${prefix}_bot`,
                credentialRef: 'secret://e2e',
                connectionMode: 'webhook',
                capabilityConfig: { e2e: true },
                status: 'active',
                createdAt: now,
                updatedAt: now,
            });
            await tx.identities.createIfAbsent({
                id: identityId,
                channelAccountId: channelId,
                externalUserIdCiphertext: `${SECRET}:${prefix}`,
                externalUserIdHash: `${prefix}_identity_hash`,
                displayName: 'e2e-fixture',
                status: 'active',
                createdAt: now,
                updatedAt: now,
            });
            await tx.bindings.createActiveIfAbsent({
                id: bindingId,
                userId,
                deviceId,
                externalIdentityId: identityId,
                priority: 10,
                status: 'active',
                boundAt: now,
            });
        });

        const actionStream = new SseActionCommandHub();
        const runtime = createImGateway({
            unitOfWork: uow,
            actionStream,
            actionTokens: new InMemoryActionTokenPort(),
            authentication: new DatabaseDeviceAuthenticationPort(uow),
            channelCapabilities: new MockChannelCapabilities(),
            channelHealth: {
                check: async (account) => ({ accountId: account.id, status: 'healthy', checkedAt: clock.now() }),
            },
            conversations: new DirectConversationResolver(),
            deliveryRenderer: new MockDeliveryRenderer(),
            imChannel: {
                send: async (message) => {
                    sends.push(message);
                    return { accepted: true, platformMessageId: `platform-${sends.length}` };
                },
            },
            pairingCodes: new MockPairingCodePort(),
            identityProtector: {
                protect: async (value) => ({ ciphertext: `protected:${value}`, hash: `hash:${value}` }),
            },
            clock,
            ids: new UuidIdGenerator(channelId),
        });
        worker = new DeliveryOutboxWorker({
            unitOfWork: uow,
            dispatch: runtime.application.deliveryDispatch,
            clock,
            logger: { log: (entry) => logs.push(entry) },
            pollIntervalMs: 25,
        });
        worker.start();
        server = await startGatewayHttpServer({
            host: '127.0.0.1',
            port: 0,
            runtime,
            logger: { log: (entry) => logs.push(entry) },
            deliveryAvailable: () => worker.wake(),
            healthCheck: async () => ({ status: 'ok' }),
        });

        const strong = notification(`${prefix}_strong`, deviceId, userId, 'strong', `${prefix}_trigger`);
        const [first, identical] = await Promise.all([
            postNotification(server.origin, strong),
            postNotification(server.origin, strong),
        ]);
        assert(first.status === 202 && identical.status === 202, 'notification_not_accepted');
        const firstBody = await first.json();
        const identicalBody = await identical.json();
        assert(isDeepStrictEqual(firstBody, identicalBody), 'identical_replay_changed_response');
        await waitFor(() => sends.length === 1);
        const deliveryId = firstBody.deliveries[0].deliveryId;
        await waitFor(
            async () => (await runtime.application.deliveries.find(deliveryId))?.delivery.status === 'accepted',
        );
        const accepted = await runtime.application.deliveries.find(deliveryId);
        assert(accepted?.delivery.status === 'accepted', 'platform_acceptance_not_recorded');
        await runtime.application.receipts.record({
            externalEventId: `${prefix}_receipt`,
            channelAccountId: channelId,
            externalMessageId: 'platform-1',
            dedupeKey: `${prefix}_receipt_dedupe`,
            stage: 'delivered',
            occurredAt: clock.now(),
        });
        const delivered = await runtime.application.deliveries.find(deliveryId);
        assert(delivered?.delivery.status === 'delivered', 'async_receipt_not_recorded');
        const conflict = await postNotification(server.origin, { ...strong, content: { title: 'conflicting replay' } });
        assert(conflict.status === 409, 'conflicting_replay_not_rejected');

        const weak = notification(`${prefix}_weak`, deviceId, userId, 'weak', `${prefix}_weak_trigger`);
        const weakResponse = await postNotification(server.origin, weak);
        assert(weakResponse.status === 202, 'weak_notification_not_accepted');
        await waitFor(() => sends.length === 2);
        assert(sends[1]?.content?.actionUi === undefined, 'weak_reminder_created_action_ui');

        const actionToken = sends[0]?.content?.actionUi?.token;
        assert(typeof actionToken === 'string', 'strong_reminder_missing_action_token');
        const targetStream = await openStream(server.origin, deviceId, `${prefix}_trigger`);
        const wrongDevice = await globalThis.fetch(
            `${server.origin}/v1/devices/${encodeURIComponent(`${prefix}_other`)}/reminder-actions/stream?reminderType=strong&reminderTriggerId=${encodeURIComponent(`${prefix}_trigger`)}`,
            { headers: authHeaders() },
        );
        assert(wrongDevice.status === 403, 'sse_device_isolation_failed');
        const actionPage = await globalThis.fetch(
            `${server.origin}/voicelife/reminder-actions/${encodeURIComponent(actionToken)}`,
            {
                method: 'POST',
                headers: { 'content-type': 'application/x-www-form-urlencoded' },
                body: new globalThis.URLSearchParams({ action: 'acknowledge' }),
            },
        );
        assert(actionPage.status === 200, 'action_submission_failed');
        const event = await targetStream.next();
        assert(event?.data?.commandId && event.data.deviceId === deviceId, 'sse_command_missing');
        await targetStream.cancel();
        const replayStream = await openStream(server.origin, deviceId, `${prefix}_trigger`, event.id);
        const replay = await replayStream.next();
        assert(replay?.id === event.id, 'last_event_id_replay_failed');
        await replayStream.cancel();

        const resultBody = {
            schemaVersion: '1',
            operationId: event.data.operationId,
            reminderTriggerId: event.data.reminderTriggerId,
            status: 'succeeded',
            occurredAt: new Date().toISOString(),
        };
        const resultPath = `${server.origin}/v1/devices/${encodeURIComponent(deviceId)}/reminder-actions/${encodeURIComponent(event.id)}/result`;
        const result = await postJson(resultPath, resultBody);
        const duplicateResult = await postJson(resultPath, resultBody);
        assert(result.status === 200 && duplicateResult.status === 200, 'action_result_not_accepted');
        const counts = await uow.runRaw(
            `SELECT
            (SELECT COUNT(*)::int FROM im_deliveries WHERE business_event_id LIKE $1) AS deliveries,
            (SELECT COUNT(*)::int FROM im_delivery_attempts
                WHERE delivery_id IN (SELECT id FROM im_deliveries WHERE business_event_id LIKE $1)) AS attempts,
            (SELECT COUNT(*)::int FROM im_delivery_receipts
                WHERE delivery_id IN (SELECT id FROM im_deliveries WHERE business_event_id LIKE $1)) AS receipts,
            (SELECT COUNT(*)::int FROM im_actions WHERE device_id = $2) AS actions`,
            [`${prefix}%`, deviceId],
        );
        assert(
            counts[0]?.deliveries === 2 &&
                counts[0]?.attempts === 2 &&
                counts[0]?.receipts === 1 &&
                counts[0]?.actions === 1,
            'persistence_counts_mismatch',
        );
        const serializedLogs = JSON.stringify(logs);
        assert(
            !serializedLogs.includes(DEVICE_TOKEN) && !serializedLogs.includes(actionToken),
            'sensitive_evidence_leaked',
        );
        return {
            assertions: 10,
            deliveryCount: counts[0]?.deliveries ?? 0,
            sendCount: sends.length,
            receiptCount: counts[0]?.receipts ?? 0,
            actionCount: counts[0]?.actions ?? 0,
        };
    } finally {
        if (server !== undefined) await server.close().catch(() => undefined);
        if (worker !== undefined) await worker.close().catch(() => undefined);
        if (uow !== undefined) await uow.close().catch(() => undefined);
        if (schemaCreated) await admin.query(`DROP SCHEMA "${prefix}" CASCADE`).catch(() => undefined);
        await admin.end().catch(() => undefined);
    }
}

function notification(eventId, deviceId, userId, reminderType, triggerId) {
    return {
        schemaVersion: '1',
        businessEventId: eventId,
        correlationId: `${eventId}_correlation`,
        kind: 'reminder_due',
        recipient: { userId, deviceId },
        scheduleId: `${eventId}_schedule`,
        taskId: `${eventId}_task`,
        instanceId: `${eventId}_instance`,
        reminderTriggerId: triggerId,
        reminderType,
        content: { title: `${reminderType} fixture` },
        plannedAt: new Date().toISOString(),
        triggerAt: new Date().toISOString(),
        occurredAt: new Date().toISOString(),
        actions: reminderType === 'strong' ? [{ kind: 'command', type: 'acknowledge', label: 'Acknowledge' }] : [],
    };
}

function authHeaders() {
    return { authorization: `Bearer ${DEVICE_TOKEN}` };
}

async function postNotification(origin, body) {
    return postJson(`${origin}/v1/im/notifications`, body, { 'idempotency-key': body.businessEventId });
}

async function postJson(url, body, extra = {}) {
    return globalThis.fetch(url, {
        method: 'POST',
        headers: { ...authHeaders(), 'content-type': 'application/json', ...extra },
        body: JSON.stringify(body),
    });
}

async function openStream(origin, deviceId, triggerId, lastEventId) {
    const url = `${origin}/v1/devices/${encodeURIComponent(deviceId)}/reminder-actions/stream?reminderType=strong&reminderTriggerId=${encodeURIComponent(triggerId)}`;
    const response = await globalThis.fetch(url, {
        headers: { ...authHeaders(), ...(lastEventId === undefined ? {} : { 'last-event-id': lastEventId }) },
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

async function waitFor(predicate, timeoutMs = 5000) {
    const deadline = Date.now() + timeoutMs;
    while (!(await predicate())) {
        if (Date.now() >= deadline) throw new Error('journey_timeout');
        await new Promise((resolve) => globalThis.setTimeout(resolve, 10));
    }
}

function assert(condition, code) {
    if (!condition) throw new Error(code);
}

if (import.meta.url === `file://${process.argv[1]}`) {
    runHostE2e({ runId: process.env.E2E_RUN_ID })
        .then((result) => process.stdout.write(`${JSON.stringify(result)}\n`))
        .catch((error) => {
            const infrastructure =
                error !== null &&
                typeof error === 'object' &&
                'code' in error &&
                ['ECONNREFUSED', 'ENOTFOUND', 'ETIMEDOUT'].includes(error.code);
            process.stderr.write(infrastructure ? 'host_e2e_infrastructure_failed\n' : 'host_e2e_failed\n');
            process.exitCode = 1;
        });
}
