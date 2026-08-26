import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';

import { tokenDigest } from '../dist/application/device-management.js';
import {
    readGatewayConfiguration,
    startConfiguredGatewayProcess,
    startGatewayHttpServer,
} from '../dist/app/gateway-process.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';
import { AesGcmExternalIdentityProtector } from '../dist/infrastructure/security/production-ports.js';
import { ImGatewayError } from '../dist/shared/errors.js';

const deviceToken = 'fixture-device-token-with-enough-entropy';

class FakeWecomWebSocket {
    sent = [];
    listeners = new Map();

    addEventListener(type, listener) {
        const listeners = this.listeners.get(type) ?? [];
        listeners.push(listener);
        this.listeners.set(type, listeners);
    }

    send(data) {
        this.sent.push(JSON.parse(data));
    }

    close() {
        this.emit('close', {});
    }

    emit(type, event) {
        for (const listener of this.listeners.get(type) ?? []) listener(event);
    }
}

async function waitFor(assertion, message) {
    const deadline = Date.now() + 2_000;
    while (Date.now() < deadline) {
        if (await assertion()) return;
        await new Promise((resolve) => globalThis.setTimeout(resolve, 10));
    }
    assert.fail(message);
}

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
        WECHAT_QUERY_TEMPLATE_ID: 'fixture-query-template',
        WECHAT_QUERY_TEMPLATE_TITLE_FIELD: 'first',
        WECHAT_QUERY_TEMPLATE_BODY_FIELD: 'keyword1',
        WECHAT_QUERY_TEMPLATE_TIME_FIELD: 'keyword2',
        WECHAT_ACTION_UI_BASE_URL: 'https://gateway.example/voicelife/reminder-actions',
        ...overrides,
    };
}

function isPostgresUnavailable(error) {
    return (
        error !== null &&
        typeof error === 'object' &&
        'code' in error &&
        ['ECONNREFUSED', 'ENOTFOUND', 'ETIMEDOUT'].includes(error.code)
    );
}

function fakeRuntime(events) {
    return {
        deviceApi: {
            postPairingSession: async (input) => ({ session: { id: 'pairing-1' }, displayCode: input.body.deviceId }),
            getPairingSession: async (input) => ({ id: input.pairingSessionId }),
            postScheduleReceipt: async (input) => ({ accepted: true, deliveries: [], eventId: input.body.eventId }),
            postScheduleQueryResult: async (input) => ({
                accepted: true,
                deliveries: [{ deliveryId: 'query-delivery-1', status: 'pending' }],
                businessEventId: input.body.businessEventId,
            }),
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
            postReminderActionStatusReport: async (input) => ({
                accepted: true,
                eventId: input.body.eventId,
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
        scheduleQueryPageApi: {
            get: async (token) => ({
                status: 200,
                headers: { 'content-type': 'text/html; charset=utf-8' },
                body: `<h1>日程 ${token}</h1>`,
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

test('production server accepts a complete schedule query result through the device route', async () => {
    const received = [];
    const runtime = fakeRuntime([]);
    runtime.deviceApi.postScheduleQueryResult = async (input) => {
        received.push(input);
        return {
            accepted: true,
            deliveries: [{ deliveryId: 'query-delivery-1', status: 'pending' }],
            businessEventId: input.body.businessEventId,
        };
    };
    await withServer(
        async ({ origin, events, logs }) => {
            const body = { businessEventId: 'query-event-1', correlationId: 'query-correlation-1' };
            const response = await globalThis.fetch(`${origin}/v1/im/schedule-query-results`, {
                method: 'POST',
                headers: {
                    authorization: 'Bearer fixture-device-token',
                    'content-type': 'application/json',
                    'idempotency-key': 'query-event-1',
                },
                body: JSON.stringify(body),
            });
            assert.equal(response.status, 202);
            assert.deepEqual(await response.json(), {
                accepted: true,
                deliveries: [{ deliveryId: 'query-delivery-1', status: 'pending' }],
                businessEventId: 'query-event-1',
            });
            assert.deepEqual(received, [
                {
                    authorization: 'Bearer fixture-device-token',
                    idempotencyKey: 'query-event-1',
                    body,
                },
            ]);
            assert.deepEqual(events, [{ kind: 'worker-wake' }]);
            assert.equal(logs.at(-1).route, 'device.schedule-query-result.create');
            assert.equal(logs.at(-1).correlationId, 'query-correlation-1');
        },
        { runtime },
    );
});

test('production server serves schedule query pages as read-only routes', async () => {
    await withServer(async ({ origin, logs }) => {
        const page = await globalThis.fetch(`${origin}/voicelife/reminder-actions/query-result/token%2Evalue`);
        assert.equal(page.status, 200);
        assert.equal(page.headers.get('content-type'), 'text/html; charset=utf-8');
        assert.equal(await page.text(), '<h1>日程 token.value</h1>');
        assert.equal(logs.at(-1).route, 'schedule-query-page');

        const write = await globalThis.fetch(`${origin}/voicelife/reminder-actions/query-result/token%2Evalue`, {
            method: 'POST',
        });
        assert.equal(write.status, 405);
        assert.equal(write.headers.get('allow'), 'GET');
        assert.equal(logs.at(-1).route, 'schedule-query-page');
    });
});

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
                WECOM_AIBOT_SECRET: 'secret-fixture',
            }),
        ).wecom,
        { channelAccountId: 'wecom-production', botId: 'bot-fixture', secret: 'secret-fixture' },
    );
    assert.throws(
        () => readGatewayConfiguration(fixtureEnvironment({ WECOM_AIBOT_BOT_ID: 'bot-fixture' })),
        /WECOM_AIBOT_CHANNEL_ACCOUNT_ID is required/u,
    );
    assert.throws(
        () => readGatewayConfiguration(fixtureEnvironment({ WECOM_AIBOT_SECRET: 'secret-fixture' })),
        /WECOM_AIBOT_CHANNEL_ACCOUNT_ID is required/u,
    );
});

test('production configuration rejects unsafe transport and endpoint settings', () => {
    for (const [overrides, message] of [
        [{ GATEWAY_PORT: '65536' }, 'GATEWAY_PORT must be a valid TCP port'],
        [{ WECHAT_WEBHOOK_MODE: 'encrypted' }, 'WECHAT_WEBHOOK_MODE must be plain for the current adapter'],
        [
            { WECHAT_EXPECTED_TO_USERNAME: 'wechat-production' },
            'WECHAT_EXPECTED_TO_USERNAME must be the gh_ prefixed original account ID',
        ],
        [
            { WECHAT_TEMPLATE_TITLE_FIELD: '1invalid' },
            'WECHAT_TEMPLATE_TITLE_FIELD must be a valid WeChat template field name',
        ],
        [{ DATABASE_URL: 'https://database.example/voicelife' }, 'DATABASE_URL must be a PostgreSQL connection URL'],
        [{ DATABASE_HOST: 'database host' }, 'DATABASE_HOST must be a valid hostname'],
        [
            { WECHAT_ACTION_UI_BASE_URL: 'http://gateway.example/voicelife/reminder-actions' },
            'WECHAT_ACTION_UI_BASE_URL must be a public HTTPS Action UI base URL',
        ],
    ]) {
        assert.throws(() => readGatewayConfiguration(fixtureEnvironment(overrides)), new RegExp(message, 'u'));
    }
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

test('production entry classifies unexpected startup failures without exposing configuration values', async () => {
    const secret = 'secret-value-that-must-not-be-logged';
    const child = spawn(process.execPath, ['scripts/start-gateway.mjs'], {
        cwd: new URL('..', import.meta.url),
        env: {
            ...process.env,
            ...fixtureEnvironment({
                DATABASE_URL: 'postgres://user:password@127.0.0.1:1/voicelife',
                ACTION_TOKEN_SECRET: secret,
            }),
        },
        stdio: ['ignore', 'ignore', 'pipe'],
    });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => {
        stderr += chunk;
    });
    const [exitCode] = await once(child, 'exit');

    assert.equal(exitCode, 1);
    assert.doesNotMatch(stderr, new RegExp(secret, 'u'));
    assert.deepEqual(JSON.parse(stderr), {
        level: 'error',
        event: 'gateway.start.failed',
        errorCode: 'startup_failed',
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

        const actionStatusReport = await globalThis.fetch(
            `${origin}/v1/devices/device-fixture/reminder-action-status`,
            {
                method: 'POST',
                headers: {
                    authorization: `Bearer ${deviceToken}`,
                    'content-type': 'application/json',
                    'idempotency-key': 'voice-event-1',
                },
                body: JSON.stringify({
                    schemaVersion: '1',
                    eventId: 'voice-event-1',
                    correlationId: 'voice-correlation-1',
                    deviceId: 'device-fixture',
                    reminderTriggerId: 'trigger-fixture',
                    operationId: 'voice-operation-1',
                    action: 'acknowledge',
                    status: 'succeeded',
                    occurredAt: '2026-08-03T00:01:00.000Z',
                    source: 'voice',
                }),
            },
        );
        assert.equal(actionStatusReport.status, 202);
        assert.equal((await actionStatusReport.json()).eventId, 'voice-event-1');

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
        if (isPostgresUnavailable(error)) {
            context.skip(`PostgreSQL unavailable: ${error instanceof Error ? error.name : 'unknown'}`);
            return;
        }
        throw error;
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

test('production start script gracefully closes the gateway on SIGTERM', async (context) => {
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

    const child = spawn(process.execPath, ['scripts/start-gateway.mjs'], {
        cwd: new URL('..', import.meta.url),
        env: {
            ...process.env,
            ...fixtureEnvironment({
                DATABASE_URL: databaseUrl,
                GATEWAY_PORT: '0',
                WECHAT_CHANNEL_ACCOUNT_ID: `wechat-script-${Date.now()}`,
            }),
        },
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => {
        stdout += chunk;
    });
    child.stderr.on('data', (chunk) => {
        stderr += chunk;
    });

    try {
        await waitFor(() => stdout.includes('"gateway.started"'), 'start script did not report a running gateway');
        const childExited = once(child, 'exit').then(([exitCode]) => exitCode);
        assert.equal(child.kill('SIGTERM'), true);
        let timeout;
        const exitCode = await Promise.race([
            childExited,
            new Promise((resolve) => {
                timeout = globalThis.setTimeout(() => resolve(undefined), 2_000);
            }),
        ]);
        globalThis.clearTimeout(timeout);
        assert.notEqual(exitCode, undefined, 'start script did not exit promptly after SIGTERM');
        assert.equal(exitCode, 0, stderr);
        const events = stdout
            .trim()
            .split('\n')
            .map((line) => JSON.parse(line).event);
        assert.equal(events.includes('gateway.started'), true);
        assert.equal(events.includes('gateway.stopped'), true);
    } finally {
        if (child.exitCode === null && child.signalCode === null) {
            child.kill('SIGKILL');
            await once(child, 'exit');
        }
    }
});

test('configured production process registers and starts an optional WeCom AI Bot channel', async (context) => {
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

    const socket = new FakeWecomWebSocket();
    const wecomChannelId = `wecom-process-${Date.now()}`;
    const gateway = await startConfiguredGatewayProcess(
        fixtureEnvironment({
            DATABASE_URL: databaseUrl,
            GATEWAY_PORT: '0',
            WECHAT_CHANNEL_ACCOUNT_ID: `wechat-process-${Date.now()}`,
            WECOM_AIBOT_CHANNEL_ACCOUNT_ID: wecomChannelId,
            WECOM_AIBOT_BOT_ID: 'bot-fixture',
            WECOM_AIBOT_SECRET: 'secret-fixture',
        }),
        { log: () => {} },
        { createWecomWebSocket: () => socket },
    );
    try {
        socket.emit('open', {});
        const subscription = socket.sent[0];
        assert.equal(subscription.cmd, 'aibot_subscribe');
        socket.emit('message', {
            data: JSON.stringify({ headers: { req_id: subscription.headers.req_id }, errcode: 0 }),
        });
        await new Promise((resolve) => globalThis.setTimeout(resolve, 0));
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
                    connectionMode: 'websocket',
                },
            );
        } finally {
            await check.close();
        }
    } finally {
        await gateway.close();
    }
});

test('configured WeCom AI Bot sends one weak reminder and persists the accepted platform message', async (context) => {
    const databaseUrl = process.env.DATABASE_URL ?? 'postgres://voicelife:voicelife@127.0.0.1:5432/voicelife';
    const suffix = String(Date.now());
    const deviceId = `device-wecom-${suffix}`;
    const userId = `user-wecom-${suffix}`;
    const channelAccountId = `wecom-delivery-${suffix}`;
    const deviceToken = randomBytes(32).toString('base64url');
    const identitySecret = 'fixture-identity-secret-with-at-least-32-bytes';
    const setup = new PostgresImUnitOfWork(databaseUrl);
    try {
        await setup.migrate();
        await setup.transaction((tx) =>
            tx.devices.create({
                deviceId,
                userId,
                tokenDigest: tokenDigest(deviceToken),
                status: 'active',
                createdAt: '2026-08-20T00:00:00.000Z',
                updatedAt: '2026-08-20T00:00:00.000Z',
            }),
        );
    } catch (error) {
        await setup.close().catch(() => undefined);
        if (isPostgresUnavailable(error)) {
            context.skip(`PostgreSQL unavailable: ${error instanceof Error ? error.name : 'unknown'}`);
            return;
        }
        throw error;
    }
    await setup.close();

    const socket = new FakeWecomWebSocket();
    const gateway = await startConfiguredGatewayProcess(
        fixtureEnvironment({
            DATABASE_URL: databaseUrl,
            GATEWAY_PORT: '0',
            WECHAT_CHANNEL_ACCOUNT_ID: `wechat-delivery-${suffix}`,
            WECOM_AIBOT_CHANNEL_ACCOUNT_ID: channelAccountId,
            WECOM_AIBOT_BOT_ID: 'bot-fixture',
            WECOM_AIBOT_SECRET: 'secret-fixture',
        }),
        { log: () => {} },
        { createWecomWebSocket: () => socket },
    );
    try {
        socket.emit('open', {});
        const subscription = socket.sent[0];
        socket.emit('message', {
            data: JSON.stringify({ headers: { req_id: subscription.headers.req_id }, errcode: 0 }),
        });

        const protector = new AesGcmExternalIdentityProtector(identitySecret);
        const protectedIdentity = await protector.protect('userid-fixture');
        const records = new PostgresImUnitOfWork(databaseUrl);
        try {
            await records.transaction(async (tx) => {
                await tx.identities.save({
                    id: `identity-wecom-${suffix}`,
                    channelAccountId,
                    externalUserIdCiphertext: protectedIdentity.ciphertext,
                    externalUserIdHash: protectedIdentity.hash,
                    status: 'active',
                    createdAt: '2026-08-20T00:00:00.000Z',
                    updatedAt: '2026-08-20T00:00:00.000Z',
                });
                await tx.bindings.save({
                    id: `binding-wecom-${suffix}`,
                    userId,
                    deviceId,
                    externalIdentityId: `identity-wecom-${suffix}`,
                    priority: 10,
                    status: 'active',
                    boundAt: '2026-08-20T00:00:00.000Z',
                });
            });
        } finally {
            await records.close();
        }

        const notification = {
            schemaVersion: '1',
            businessEventId: `event-wecom-${suffix}`,
            correlationId: `correlation-wecom-${suffix}`,
            kind: 'reminder_due',
            recipient: { userId, deviceId },
            scheduleId: `schedule-wecom-${suffix}`,
            taskId: `task-wecom-${suffix}`,
            instanceId: `instance-wecom-${suffix}`,
            reminderTriggerId: `trigger-wecom-${suffix}`,
            reminderType: 'weak',
            content: { title: '日程提醒', body: '该处理了' },
            plannedAt: '2026-08-20T00:00:00.000Z',
            triggerAt: '2026-08-20T00:00:00.000Z',
            actions: [],
            occurredAt: '2026-08-20T00:00:00.000Z',
        };
        const request = () =>
            globalThis.fetch(`${gateway.origin}/v1/im/notifications`, {
                method: 'POST',
                headers: {
                    authorization: `Bearer ${deviceToken}`,
                    'content-type': 'application/json',
                    'idempotency-key': notification.businessEventId,
                },
                body: JSON.stringify(notification),
            });
        const first = await request();
        const second = await request();
        assert.equal(first.status, 202);
        assert.equal(second.status, 202);
        const firstSubmission = await first.json();
        const secondSubmission = await second.json();
        assert.deepEqual(secondSubmission.deliveries, firstSubmission.deliveries);

        await waitFor(
            () => socket.sent.some((frame) => frame.cmd === 'aibot_send_msg'),
            'WeCom reminder was not sent through WSS',
        );
        const sent = socket.sent.filter((frame) => frame.cmd === 'aibot_send_msg');
        assert.equal(sent.length, 1);
        assert.deepEqual(sent[0].body, {
            chatid: 'userid-fixture',
            msgtype: 'markdown',
            markdown: { content: '**日程提醒**\n该处理了' },
        });
        socket.emit('message', {
            data: JSON.stringify({
                headers: { req_id: sent[0].headers.req_id },
                errcode: 0,
                body: { msgid: `platform-wecom-${suffix}` },
            }),
        });

        const deliveryId = firstSubmission.deliveries[0].deliveryId;
        await waitFor(async () => {
            const check = new PostgresImUnitOfWork(databaseUrl);
            try {
                return await check.transaction(async (tx) => {
                    const delivery = await tx.deliveries.findById(deliveryId);
                    return delivery?.status === 'accepted';
                });
            } finally {
                await check.close();
            }
        }, 'WeCom platform acceptance was not persisted');
        const check = new PostgresImUnitOfWork(databaseUrl);
        try {
            const { delivery, attempts } = await check.transaction(async (tx) => ({
                delivery: await tx.deliveries.findById(deliveryId),
                attempts: await tx.deliveries.listAttempts(deliveryId),
            }));
            assert.equal(delivery?.channelAccountId, channelAccountId);
            assert.equal(delivery?.externalMessageId, `platform-wecom-${suffix}`);
            assert.deepEqual(
                attempts.map((attempt) => ({ status: attempt.status, platformMessageId: attempt.platformMessageId })),
                [{ status: 'accepted', platformMessageId: `platform-wecom-${suffix}` }],
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
