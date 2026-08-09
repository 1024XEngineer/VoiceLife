import assert from 'node:assert/strict';
import { test } from 'node:test';

import { readGatewayConfiguration, startGatewayHttpServer } from '../dist/app/gateway-process.js';
import { ImGatewayError } from '../dist/shared/errors.js';

const deviceToken = 'fixture-device-token-with-enough-entropy';

function fixtureEnvironment(overrides = {}) {
    return {
        DATABASE_URL: 'postgres://user:password@postgres:5432/voicelife',
        GATEWAY_HOST: '127.0.0.1',
        GATEWAY_PORT: '3000',
        DEVICE_ID: 'device-fixture',
        DEVICE_TOKEN: deviceToken,
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
            postReminderActionResult: async (input) => ({ id: input.commandId, status: input.body.status }),
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
            post: async () => 'success',
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

async function withServer(work) {
    const events = [];
    const logs = [];
    const server = await startGatewayHttpServer({
        host: '127.0.0.1',
        port: 0,
        runtime: fakeRuntime(events),
        healthCheck: async () => ({ status: 'ok' }),
        logger: { log: (entry) => logs.push(entry) },
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
    assert.equal(
        new URL(readGatewayConfiguration(fixtureEnvironment({ DATABASE_HOST: 'postgres' })).databaseUrl).hostname,
        'postgres',
    );

    assert.throws(
        () => readGatewayConfiguration(fixtureEnvironment({ WECHAT_APP_SECRET: '' })),
        /WECHAT_APP_SECRET is required/u,
    );
    assert.throws(
        () => readGatewayConfiguration(fixtureEnvironment({ ACTION_TOKEN_SECRET: 'too-short' })),
        /ACTION_TOKEN_SECRET must contain at least 32 bytes/u,
    );
});

test('production server returns a Bearer challenge for rejected device credentials', async () => {
    const runtime = fakeRuntime([]);
    runtime.deviceApi.postNotification = async () => {
        throw new ImGatewayError('unauthorized', 'fixture credential rejected');
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
        assert.deepEqual(await response.json(), { error: 'unauthorized' });
    } finally {
        await server.close();
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

        await new Promise((resolve) => globalThis.setImmediate(resolve));
        assert.deepEqual(events, [{ kind: 'dispatch', deliveryId: 'delivery-1' }]);

        const actionPage = await globalThis.fetch(`${origin}/voicelife/reminder-actions/token%2Evalue`);
        assert.equal(actionPage.status, 200);
        assert.equal(await actionPage.text(), '<p>token.value</p>');

        const webhook = await globalThis.fetch(`${origin}/wechat?echostr=challenge&signature=s&timestamp=1&nonce=n`);
        assert.equal(webhook.status, 200);
        assert.equal(await webhook.text(), 'challenge');
    });
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
