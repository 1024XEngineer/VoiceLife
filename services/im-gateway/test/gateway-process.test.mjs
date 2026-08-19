import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';
import { TextDecoder } from 'node:util';

import {
    readGatewayConfiguration,
    startConfiguredGatewayProcess,
    startGatewayHttpServer,
} from '../dist/app/gateway-process.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';
import { ImGatewayError } from '../dist/shared/errors.js';

const deviceToken = 'fixture-device-token-with-enough-entropy';

function fixtureEnvironment(overrides = {}) {
    return {
        DATABASE_URL: 'postgres://user:password@postgres:5432/voicelife',
        GATEWAY_HOST: '127.0.0.1',
        GATEWAY_PORT: '3000',
        ACTION_TOKEN_SECRET: 'fixture-action-token-secret-with-32-bytes',
        IDENTITY_SECRET: 'fixture-identity-secret-with-at-least-32-bytes',
        WECHAT_CHANNEL_ACCOUNT_ID: 'wechat-production',
        WECHAT_APP_ID: 'wx-fixture',
        WECHAT_APP_SECRET: 'fixture-app-secret',
        WECHAT_WEBHOOK_TOKEN: 'fixture-webhook-token',
        WECHAT_EXPECTED_TO_USERNAME: 'gh_fixture',
        WECHAT_TEMPLATE_ID: 'fixture-template',
        WECHAT_TEMPLATE_TITLE_FIELD: 'first',
        WECHAT_TEMPLATE_BODY_FIELD: 'keyword1',
        WECHAT_TEMPLATE_TIME_FIELD: 'keyword2',
        WECHAT_ACTION_UI_BASE_URL: 'https://gateway.example/voicelife/reminder-actions',
        ...overrides,
    };
}

function fakeRuntime(events) {
    return {
        deviceApi: {
            postPairingSession: async (input) => ({ session: { id: 'pairing-1' }, displayCode: input.body.deviceId }),
            getPairingSession: async (input) => ({ id: input.pairingSessionId }),
            postScheduleReceipt: async (input) => ({ accepted: true, deliveries: [], eventId: input.body.eventId }),
            postNotification: async (input) => ({
                accepted: true,
                deliveries: [{ deliveryId: 'delivery-1', status: 'pending' }],
                eventId: input.body.businessEventId,
            }),
            postReminderActionResult: async (input) => ({
                id: input.commandId,
                status: input.body.status,
                correlationId: 'correlation-action-result',
            }),
        },
        actionStreamApi: {
            connect: async () =>
                (async function* actionEvents() {
                    yield {
                        id: 'action-1',
                        event: 'reminder.action',
                        data: {
                            commandId: 'action-1',
                            correlationId: 'correlation-stream',
                            action: 'acknowledge',
                        },
                    };
                })(),
        },
        actionUiPageApi: {
            get: async (token) => ({
                status: 200,
                headers: { 'content-type': 'text/html; charset=utf-8' },
                body: `<p>${token}</p>`,
            }),
            post: async () => ({
                status: 200,
                headers: { 'content-type': 'text/html; charset=utf-8' },
                body: '<p>submitted</p>',
            }),
        },
        wechatApi: {
            verify: (input) => input.echostr,
            post: async () => ({ body: 'success', contentType: 'text/plain; charset=utf-8' }),
        },
        application: {
            deliveryDispatch: {
                dispatch: async (deliveryId) => {
                    events.push({ kind: 'dispatch', deliveryId });
                    return { id: deliveryId, status: 'accepted', correlationId: 'correlation-notification' };
                },
            },
        },
    };
}

async function withServer(work, options = {}) {
    const events = [];
    const logs = [];
    const server = await startGatewayHttpServer({
        host: '127.0.0.1',
        port: 0,
        runtime: fakeRuntime(events),
        healthCheck: async () => ({ status: 'ok' }),
        logger: { log: (entry) => logs.push(entry) },
        deliveryAvailable: () => events.push({ kind: 'worker-wake' }),
        ...options,
    });
    try {
        await work({ ...server, events, logs });
    } finally {
        await server.close();
    }
}

test('production server mounts the optional WeCom AI Bot URL callback', async () => {
    const received = [];
    await withServer(
        async ({ origin }) => {
            const verification = await globalThis.fetch(
                `${origin}/wecom/aibot?timestamp=1786665600&nonce=nonce-fixture&msg_signature=signature-fixture&echostr=encrypted-fixture`,
            );
            assert.equal(verification.status, 200);
            assert.equal(await verification.text(), 'url-verification');

            const callback = await globalThis.fetch(
                `${origin}/wecom/aibot?timestamp=1786665600&nonce=nonce-fixture&msg_signature=signature-fixture`,
                {
                    method: 'POST',
                    headers: { 'content-type': 'application/json' },
                    body: JSON.stringify({ encrypt: 'encrypted-fixture' }),
                },
            );
            assert.equal(callback.status, 200);
            assert.equal(await callback.text(), 'success');
        },
        {
            wecomAibotApi: {
                verify: (request) => {
                    received.push({ kind: 'verify', request });
                    return 'url-verification';
                },
                post: async (request) => {
                    received.push({
                        kind: 'post',
                        request: { ...request, body: new TextDecoder().decode(request.body) },
                    });
                    return { status: 200, body: 'success' };
                },
            },
        },
    );
    assert.deepEqual(received, [
        {
            kind: 'verify',
            request: {
                timestamp: '1786665600',
                nonce: 'nonce-fixture',
                msg_signature: 'signature-fixture',
                echostr: 'encrypted-fixture',
            },
        },
        {
            kind: 'post',
            request: {
                timestamp: '1786665600',
                nonce: 'nonce-fixture',
                msg_signature: 'signature-fixture',
                body: '{"encrypt":"encrypted-fixture"}',
            },
        },
    ]);
});

test('production configuration requires every secret without exposing its value', () => {
    const config = readGatewayConfiguration(fixtureEnvironment());
    assert.equal(config.host, '127.0.0.1');
    assert.equal(config.port, 3000);
    assert.equal(config.wechat.channelAccountId, 'wechat-production');
    assert.equal(config.wechat.displayTimeZone, 'Asia/Shanghai');
    assert.equal(config.wecom, undefined);
    assert.equal(
        new URL(readGatewayConfiguration(fixtureEnvironment({ DATABASE_HOST: 'postgres' })).databaseUrl).hostname,
        'postgres',
    );

    assert.throws(
        () => readGatewayConfiguration(fixtureEnvironment({ WECHAT_APP_SECRET: '' })),
        /WECHAT_APP_SECRET is required/u,
    );
    assert.throws(
        () => readGatewayConfiguration(fixtureEnvironment({ WECHAT_DISPLAY_TIME_ZONE: 'not/a-time-zone' })),
        /WECHAT_DISPLAY_TIME_ZONE must be a valid IANA time zone/u,
    );
    assert.throws(
        () => readGatewayConfiguration(fixtureEnvironment({ ACTION_TOKEN_SECRET: 'too-short' })),
        /ACTION_TOKEN_SECRET must contain at least 32 bytes/u,
    );
    assert.deepEqual(
        readGatewayConfiguration(
            fixtureEnvironment({
                WECOM_AIBOT_CHANNEL_ACCOUNT_ID: 'wecom-production',
                WECOM_AIBOT_BOT_ID: 'bot-fixture',
                WECOM_AIBOT_WEBHOOK_TOKEN: 'wecom-webhook-token',
                WECOM_AIBOT_ENCODING_AES_KEY: 'abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG',
            }),
        ).wecom,
        {
            channelAccountId: 'wecom-production',
            botId: 'bot-fixture',
            webhookToken: 'wecom-webhook-token',
            encodingAesKey: 'abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG',
        },
    );
    assert.throws(
        () => readGatewayConfiguration(fixtureEnvironment({ WECOM_AIBOT_BOT_ID: 'bot-fixture' })),
        /WECOM_AIBOT_CHANNEL_ACCOUNT_ID is required/u,
    );
    assert.throws(
        () => readGatewayConfiguration(fixtureEnvironment({ WECOM_AIBOT_ENCODING_AES_KEY: 'fixture-key' })),
        /WECOM_AIBOT_CHANNEL_ACCOUNT_ID is required/u,
    );
});

test('production configuration rejects current and historical public example secrets', async () => {
    for (const [name, value] of [
        ['ACTION_TOKEN_SECRET', 'replace-with-at-least-32-random-bytes'],
        ['IDENTITY_SECRET', 'replace-with-a-distinct-32-byte-random-secret'],
    ]) {
        assert.throws(
            () => readGatewayConfiguration(fixtureEnvironment({ [name]: value })),
            new RegExp(`${name} must not use the public example value`, 'u'),
        );
    }

    const example = Object.fromEntries(
        (await readFile(new URL('../../../.env.example', import.meta.url), 'utf8'))
            .split(/\r?\n/u)
            .filter((line) => line !== '' && !line.startsWith('#'))
            .map((line) => line.split('=', 2)),
    );
    for (const name of ['ACTION_TOKEN_SECRET', 'IDENTITY_SECRET']) {
        assert.throws(
            () => readGatewayConfiguration(fixtureEnvironment({ [name]: example[name] })),
            (error) => error.name === 'GatewayConfigurationError' && error.message.startsWith(`${name} `),
        );
    }
});

test('production entry reports trusted configuration errors without logging their values', async () => {
    const leakedValue = 'secret-value-that-is-too-short';
    const child = spawn(process.execPath, ['scripts/start-gateway.mjs'], {
        cwd: new URL('..', import.meta.url),
        env: { ...process.env, ...fixtureEnvironment({ ACTION_TOKEN_SECRET: leakedValue }) },
        stdio: ['ignore', 'ignore', 'pipe'],
    });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => {
        stderr += chunk;
    });
    const [exitCode] = await once(child, 'exit');

    assert.equal(exitCode, 1);
    assert.doesNotMatch(stderr, new RegExp(leakedValue, 'u'));
    assert.deepEqual(JSON.parse(stderr), {
        level: 'error',
        event: 'gateway.start.failed',
        errorCode: 'invalid_configuration',
        message: 'ACTION_TOKEN_SECRET must contain at least 32 bytes',
    });
});

test('production server returns a Bearer challenge for rejected device credentials', async () => {
    const runtime = fakeRuntime([]);
    runtime.deviceApi.postNotification = async () => {
        throw new ImGatewayError('unauthorized', '<img src=x onerror=alert(1)>');
    };
    const server = await startGatewayHttpServer({
        host: '127.0.0.1',
        port: 0,
        runtime,
        healthCheck: async () => ({ status: 'ok' }),
        logger: { log: () => {} },
    });
    try {
        const response = await globalThis.fetch(`${server.origin}/v1/im/notifications`, {
            method: 'POST',
            headers: {
                authorization: 'Bearer invalid-token',
                'content-type': 'application/json',
                'idempotency-key': 'event-1',
            },
            body: JSON.stringify({ businessEventId: 'event-1', correlationId: 'correlation-1' }),
        });
        assert.equal(response.status, 401);
        assert.equal(response.headers.get('www-authenticate'), 'Bearer');
        assert.equal(await response.text(), '{"error":"unauthorized"}');
    } finally {
        await server.close();
    }
});

test('production server maps every gateway error to a stable JSON response', async () => {
    const runtime = fakeRuntime([]);
    let errorCode = 'unauthorized';
    runtime.deviceApi.postNotification = async () => {
        throw new ImGatewayError(errorCode, '<img src=x onerror=alert(1)>');
    };
    const server = await startGatewayHttpServer({
        host: '127.0.0.1',
        port: 0,
        runtime,
        healthCheck: async () => ({ status: 'ok' }),
        logger: { log: () => {} },
    });
    const cases = [
        ['unauthorized', 401],
        ['invalid_contract', 400],
        ['pairing_code_invalid', 400],
        ['capability_not_supported', 400],
        ['not_implemented', 400],
        ['binding_not_found', 404],
        ['delivery_not_found', 404],
        ['action_not_found', 404],
        ['action_expired', 410],
        ['idempotency_conflict', 409],
        ['duplicate_event', 409],
        ['invalid_transition', 403],
        ['resource_exhausted', 429],
    ];
    try {
        for (const [code, status] of cases) {
            errorCode = code;
            const response = await globalThis.fetch(`${server.origin}/v1/im/notifications`, {
                method: 'POST',
                headers: {
                    authorization: 'Bearer invalid-token',
                    'content-type': 'application/json',
                    'idempotency-key': 'event-1',
                },
                body: JSON.stringify({ businessEventId: 'event-1' }),
            });
            assert.equal(response.status, status);
            assert.equal(await response.text(), `{"error":"${code}"}`);
        }
    } finally {
        await server.close();
    }
});

test('production server reports exhausted SSE capacity as too many requests', async () => {
    const runtime = fakeRuntime([]);
    runtime.actionStreamApi.connect = async () => {
        throw new ImGatewayError('resource_exhausted', 'fixture capacity reached', true);
    };
    const server = await startGatewayHttpServer({
        host: '127.0.0.1',
        port: 0,
        runtime,
        healthCheck: async () => ({ status: 'ok' }),
        logger: { log: () => {} },
    });
    try {
        const response = await globalThis.fetch(
            `${server.origin}/v1/devices/device-fixture/reminder-actions/stream?reminderType=strong&reminderTriggerId=trigger-1`,
            { headers: { authorization: `Bearer ${deviceToken}` } },
        );
        assert.equal(response.status, 429);
        assert.deepEqual(await response.json(), { error: 'resource_exhausted' });
    } finally {
        await server.close();
    }
});

test('production server rejects invalid SSE heartbeat configuration before committing headers', async () => {
    for (const heartbeatIntervalMs of [0, Number.NaN, 2_147_483_648]) {
        const server = await startGatewayHttpServer({
            host: '127.0.0.1',
            port: 0,
            runtime: fakeRuntime([]),
            healthCheck: async () => ({ status: 'ok' }),
            logger: { log: () => {} },
            sseHeartbeatIntervalMs: heartbeatIntervalMs,
        });
        try {
            const response = await globalThis.fetch(
                `${server.origin}/v1/devices/device-fixture/reminder-actions/stream?reminderType=strong&reminderTriggerId=trigger-1`,
                { headers: { authorization: `Bearer ${deviceToken}` } },
            );
            assert.equal(response.status, 400);
            assert.equal(await response.text(), 'Bad Request');
        } finally {
            await server.close();
        }
    }
});

test('production server mounts health, device, Action UI and webhook routes', async () => {
    await withServer(async ({ origin, events }) => {
        const health = await globalThis.fetch(`${origin}/healthz`);
        assert.equal(health.status, 200);
        assert.deepEqual(await health.json(), { status: 'ok' });

        const notification = await globalThis.fetch(`${origin}/v1/im/notifications`, {
            method: 'POST',
            headers: {
                authorization: `Bearer ${deviceToken}`,
                'content-type': 'application/json',
                'idempotency-key': 'event-1',
            },
            body: JSON.stringify({
                businessEventId: 'event-1',
                correlationId: 'correlation-notification',
            }),
        });
        assert.equal(notification.status, 202);
        assert.equal((await notification.json()).eventId, 'event-1');

        const pairing = await globalThis.fetch(`${origin}/v1/im/pairing-sessions`, {
            method: 'POST',
            headers: { authorization: `Bearer ${deviceToken}`, 'content-type': 'application/json' },
            body: JSON.stringify({ deviceId: 'device-fixture' }),
        });
        assert.equal(pairing.status, 201);
        assert.equal((await pairing.json()).displayCode, 'device-fixture');

        const pairingStatus = await globalThis.fetch(`${origin}/v1/im/pairing-sessions/pairing%2E1`, {
            headers: { authorization: `Bearer ${deviceToken}` },
        });
        assert.equal(pairingStatus.status, 200);
        assert.deepEqual(await pairingStatus.json(), { id: 'pairing.1' });

        const scheduleReceipt = await globalThis.fetch(`${origin}/v1/im/schedule-receipts`, {
            method: 'POST',
            headers: {
                authorization: `Bearer ${deviceToken}`,
                'content-type': 'application/json',
                'idempotency-key': 'schedule-event-1',
            },
            body: JSON.stringify({ eventId: 'schedule-event-1', correlationId: 'correlation-schedule' }),
        });
        assert.equal(scheduleReceipt.status, 202);

        const actionResult = await globalThis.fetch(
            `${origin}/v1/devices/device-fixture/reminder-actions/action%2E1/result`,
            {
                method: 'POST',
                headers: { authorization: `Bearer ${deviceToken}`, 'content-type': 'application/json' },
                body: JSON.stringify({ status: 'succeeded' }),
            },
        );
        assert.equal(actionResult.status, 200);
        assert.equal((await actionResult.json()).correlationId, 'correlation-action-result');

        assert.deepEqual(events, [{ kind: 'worker-wake' }]);

        const actionPage = await globalThis.fetch(`${origin}/voicelife/reminder-actions/token%2Evalue`);
        assert.equal(actionPage.status, 200);
        assert.equal(await actionPage.text(), '<p>token.value</p>');

        const actionSubmission = await globalThis.fetch(`${origin}/voicelife/reminder-actions/token%2Evalue`, {
            method: 'POST',
            headers: { 'content-type': 'application/x-www-form-urlencoded' },
            body: new globalThis.URLSearchParams({ action: 'acknowledge' }),
        });
        assert.equal(actionSubmission.status, 200);
        assert.equal(await actionSubmission.text(), '<p>submitted</p>');

        const webhook = await globalThis.fetch(`${origin}/wechat?echostr=challenge&signature=s&timestamp=1&nonce=n`);
        assert.equal(webhook.status, 200);
        assert.equal(await webhook.text(), 'challenge');

        const webhookPost = await globalThis.fetch(`${origin}/wechat?signature=s&timestamp=1&nonce=n`, {
            method: 'POST',
            body: '<xml/>',
        });
        assert.equal(webhookPost.status, 200);
        assert.equal(webhookPost.headers.get('content-type'), 'text/plain; charset=utf-8');
        assert.equal(await webhookPost.text(), 'success');
    });
});

test('production server keeps JSON valid while escaping HTML metacharacters', async () => {
    const server = await startGatewayHttpServer({
        host: '127.0.0.1',
        port: 0,
        runtime: fakeRuntime([]),
        healthCheck: async () => ({ message: '</script><img src=x onerror=alert(1)>&' }),
        logger: { log: () => {} },
    });
    try {
        const response = await globalThis.fetch(`${server.origin}/healthz`);
        const body = await response.text();
        assert.equal(body.includes('</script>'), false);
        assert.equal(body.includes('<img'), false);
        assert.deepEqual(JSON.parse(body), { message: '</script><img src=x onerror=alert(1)>&' });
        assert.deepEqual(await new globalThis.Response(body).json(), {
            message: '</script><img src=x onerror=alert(1)>&',
        });
    } finally {
        await server.close();
    }
});

test('production server serializes an undefined JSON value as null', async () => {
    const server = await startGatewayHttpServer({
        host: '127.0.0.1',
        port: 0,
        runtime: fakeRuntime([]),
        healthCheck: async () => undefined,
        logger: { log: () => {} },
    });
    try {
        const response = await globalThis.fetch(`${server.origin}/healthz`);
        assert.equal(await response.text(), 'null');
    } finally {
        await server.close();
    }
});

test('production server bounds bodies and maps unsupported requests without leaking details', async () => {
    await withServer(async ({ origin }) => {
        const unsupported = await globalThis.fetch(`${origin}/v1/im/notifications`, {
            method: 'POST',
            headers: { 'content-type': 'text/plain', 'idempotency-key': 'event-1' },
            body: '{}',
        });
        assert.equal(unsupported.status, 415);

        const invalidJson = await globalThis.fetch(`${origin}/v1/im/notifications`, {
            method: 'POST',
            headers: { 'content-type': 'application/json', 'idempotency-key': 'event-1' },
            body: '{',
        });
        assert.equal(invalidJson.status, 400);

        const oversized = await globalThis.fetch(`${origin}/wechat`, {
            method: 'POST',
            body: 'x'.repeat(64 * 1024 + 1),
        });
        assert.equal(oversized.status, 413);

        const method = await globalThis.fetch(`${origin}/wechat`, { method: 'PUT' });
        assert.equal(method.status, 405);
        assert.equal(method.headers.get('allow'), 'GET, POST');

        const missing = await globalThis.fetch(`${origin}/missing`);
        assert.equal(missing.status, 404);
    });
});

test('configured production process migrates Postgres, starts Koishi and closes idempotently', async (context) => {
    const databaseUrl = process.env.DATABASE_URL ?? 'postgres://voicelife:voicelife@127.0.0.1:5432/voicelife';
    const probe = new PostgresImUnitOfWork(databaseUrl);
    try {
        await probe.migrate();
    } catch (error) {
        await probe.close().catch(() => undefined);
        context.skip(`PostgreSQL unavailable: ${error instanceof Error ? error.name : 'unknown'}`);
        return;
    }
    await probe.close();

    const logs = [];
    const environment = fixtureEnvironment({
        DATABASE_URL: databaseUrl,
        GATEWAY_PORT: '0',
        WECHAT_CHANNEL_ACCOUNT_ID: `wechat-process-${Date.now()}`,
    });
    const gateway = await startConfiguredGatewayProcess(environment, { log: (entry) => logs.push(entry) });
    const health = await globalThis.fetch(`${gateway.origin}/healthz`);
    assert.equal(health.status, 200);
    await Promise.all([gateway.close(), gateway.close()]);
    assert.equal(
        logs.some((entry) => entry.event === 'gateway.started'),
        true,
    );
    assert.equal(
        logs.some((entry) => entry.event === 'gateway.stopped'),
        true,
    );
    assert.equal(
        logs.some((entry) => entry.event === 'delivery.worker.started'),
        true,
    );
    assert.equal(
        logs.some((entry) => entry.event === 'delivery.worker.stopped'),
        true,
    );

    const restarted = await startConfiguredGatewayProcess(environment, { log: () => {} });
    await restarted.close();
});

test('configured production process registers an optional WeCom AI Bot URL callback channel', async (context) => {
    const databaseUrl = process.env.DATABASE_URL ?? 'postgres://voicelife:voicelife@127.0.0.1:5432/voicelife';
    const probe = new PostgresImUnitOfWork(databaseUrl);
    try {
        await probe.migrate();
    } catch (error) {
        await probe.close().catch(() => undefined);
        context.skip(`PostgreSQL unavailable: ${error instanceof Error ? error.name : 'unknown'}`);
        return;
    }
    await probe.close();

    const wecomChannelId = `wecom-process-${Date.now()}`;
    const gateway = await startConfiguredGatewayProcess(
        fixtureEnvironment({
            DATABASE_URL: databaseUrl,
            GATEWAY_PORT: '0',
            WECHAT_CHANNEL_ACCOUNT_ID: `wechat-process-${Date.now()}`,
            WECOM_AIBOT_CHANNEL_ACCOUNT_ID: wecomChannelId,
            WECOM_AIBOT_BOT_ID: 'bot-fixture',
            WECOM_AIBOT_WEBHOOK_TOKEN: 'wecom-webhook-token',
            WECOM_AIBOT_ENCODING_AES_KEY: 'abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG',
        }),
        { log: () => {} },
    );
    try {
        assert.equal((await globalThis.fetch(`${gateway.origin}/healthz`)).status, 200);

        const check = new PostgresImUnitOfWork(databaseUrl);
        try {
            const account = await check.transaction((tx) => tx.channelAccounts.findById(wecomChannelId));
            assert.deepEqual(
                {
                    id: account?.id,
                    platform: account?.platform,
                    tenantExternalId: account?.tenantExternalId,
                    connectionMode: account?.connectionMode,
                },
                {
                    id: wecomChannelId,
                    platform: 'wecom_aibot',
                    tenantExternalId: 'bot-fixture',
                    connectionMode: 'webhook',
                },
            );
        } finally {
            await check.close();
        }
    } finally {
        await gateway.close();
    }
});

test('production server serializes action commands as SSE and logs correlation ids safely', async () => {
    await withServer(async ({ origin, logs }) => {
        const response = await globalThis.fetch(
            `${origin}/v1/devices/device-fixture/reminder-actions/stream?reminderType=strong&reminderTriggerId=trigger-1`,
            { headers: { authorization: `Bearer ${deviceToken}` } },
        );
        assert.equal(response.status, 200);
        assert.match(response.headers.get('content-type'), /^text\/event-stream/u);
        assert.match(await response.text(), /id: action-1\nevent: reminder\.action\ndata: .*correlation-stream/u);
        assert.equal(
            logs.some((entry) => entry.correlationId === 'correlation-stream'),
            true,
        );

        const serialized = JSON.stringify(logs);
        assert.doesNotMatch(serialized, /fixture-device-token/u);
        assert.doesNotMatch(serialized, /token\.value/u);
        assert.doesNotMatch(serialized, /authorization/iu);
    });
});

test('production server shutdown terminates an active SSE connection', async () => {
    const runtime = fakeRuntime([]);
    runtime.actionStreamApi.connect = async ({ signal }) => ({
        [Symbol.asyncIterator]() {
            return this;
        },
        next() {
            return new Promise((resolve) => {
                if (signal.aborted) resolve({ done: true });
                else signal.addEventListener('abort', () => resolve({ done: true }), { once: true });
            });
        },
    });
    const server = await startGatewayHttpServer({
        host: '127.0.0.1',
        port: 0,
        runtime,
        healthCheck: async () => ({ status: 'ok' }),
        logger: { log: () => {} },
    });
    const client = new globalThis.AbortController();
    const response = await globalThis.fetch(
        `${server.origin}/v1/devices/device-fixture/reminder-actions/stream?reminderType=strong&reminderTriggerId=trigger-1`,
        { headers: { authorization: `Bearer ${deviceToken}` }, signal: client.signal },
    );
    assert.equal(response.status, 200);

    const close = server.close();
    const closedPromptly = await Promise.race([
        close.then(() => true),
        new Promise((resolve) => globalThis.setTimeout(() => resolve(false), 250)),
    ]);
    client.abort();
    await close;
    assert.equal(closedPromptly, true);
});

test('production health reports dependency failures as unavailable', async () => {
    const server = await startGatewayHttpServer({
        host: '127.0.0.1',
        port: 0,
        runtime: fakeRuntime([]),
        healthCheck: async () => {
            throw new Error('database unavailable');
        },
        logger: { log: () => {} },
    });
    try {
        const response = await globalThis.fetch(`${server.origin}/healthz`);
        assert.equal(response.status, 503);
        assert.deepEqual(await response.json(), { status: 'unavailable' });
    } finally {
        await server.close();
    }
});
