import { Buffer } from 'node:buffer';
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { startWechatDevHttpHarness } from '../dist/infrastructure/http/wechat-dev-harness.js';

const deviceToken = 'fixture-device-token-with-enough-entropy';

async function withHarness(overrides, work) {
    const webhookRequests = [];
    const actionRequests = [];
    const scheduleQueryPageRequests = [];
    const harness = await startWechatDevHttpHarness({
        host: '127.0.0.1',
        port: 0,
        expectedDeviceId: 'device-fixture',
        authentication: {
            authenticate: async (authorization) => {
                if (authorization !== `Bearer ${deviceToken}`) throw new Error('unauthorized');
                return { deviceId: 'device-fixture', userId: 'user-fixture' };
            },
        },
        webhookApi: {
            verify: (request) => {
                webhookRequests.push({ method: 'GET', request });
                return request.echostr;
            },
            post: async (request) => {
                webhookRequests.push({ method: 'POST', request });
                return { body: 'success', contentType: 'text/plain; charset=utf-8' };
            },
        },
        actionUiPageApi: {
            get: async (token) => {
                actionRequests.push({ method: 'GET', token });
                return {
                    status: 200,
                    headers: { 'content-type': 'text/html; charset=utf-8', 'x-frame-options': 'SAMEORIGIN' },
                    body: `<p>${token}</p>`,
                };
            },
            post: async (token, input) => {
                actionRequests.push({ method: 'POST', token, input });
                return {
                    status: 200,
                    headers: { 'content-type': 'text/html; charset=utf-8' },
                    body: '<p>submitted</p>',
                };
            },
        },
        scheduleQueryPageApi: {
            get: async (token) => {
                scheduleQueryPageRequests.push(token);
                return {
                    status: 200,
                    headers: { 'content-type': 'text/html; charset=utf-8' },
                    body: `<h1>日程 ${token}</h1>`,
                };
            },
        },
        sendTestNotification: async () => ({
            deliveryId: 'delivery-fixture',
            status: 'accepted',
            externalMessageId: '90071992547409931234',
        }),
        inspectDelivery: async (deliveryId) => ({
            deliveryId,
            status: 'delivered',
            externalMessageId: '90071992547409931234',
            attempts: 1,
            receipts: 1,
        }),
        ...overrides,
    });
    try {
        await work({ ...harness, webhookRequests, actionRequests, scheduleQueryPageRequests });
    } finally {
        await harness.close();
    }
}

test('WeChat development harness exposes health, webhook and protected page routes', async () => {
    await withHarness({}, async ({ origin, webhookRequests, actionRequests, scheduleQueryPageRequests }) => {
        const health = await globalThis.fetch(`${origin}/healthz`);
        assert.equal(health.status, 200);
        assert.deepEqual(await health.json(), { status: 'ok' });
        assert.equal(health.headers.get('cache-control'), 'no-store');

        const verification = await globalThis.fetch(
            `${origin}/wechat?signature=signed&timestamp=1786086000&nonce=random&echostr=challenge`,
        );
        assert.equal(verification.status, 200);
        assert.equal(await verification.text(), 'challenge');
        assert.deepEqual(webhookRequests[0], {
            method: 'GET',
            request: {
                signature: 'signed',
                timestamp: '1786086000',
                nonce: 'random',
                echostr: 'challenge',
            },
        });

        const xml = '<xml><MsgType><![CDATA[text]]></MsgType></xml>';
        const webhook = await globalThis.fetch(`${origin}/wechat?signature=signed&timestamp=1786086000&nonce=random`, {
            method: 'POST',
            headers: { 'content-type': 'text/xml' },
            body: xml,
        });
        assert.equal(webhook.status, 200);
        assert.equal(webhook.headers.get('content-type'), 'text/plain; charset=utf-8');
        assert.equal(await webhook.text(), 'success');
        assert.equal(webhookRequests[1].method, 'POST');
        assert.deepEqual(
            { ...webhookRequests[1].request, body: undefined },
            { signature: 'signed', timestamp: '1786086000', nonce: 'random', body: undefined },
        );
        assert.equal(Buffer.from(webhookRequests[1].request.body).toString('utf8'), xml);

        const actionPage = await globalThis.fetch(`${origin}/voicelife/reminder-actions/token%2Evalue`);
        assert.equal(actionPage.status, 200);
        assert.equal(actionPage.headers.get('x-frame-options'), 'SAMEORIGIN');
        assert.equal(await actionPage.text(), '<p>token.value</p>');

        const submitted = await globalThis.fetch(`${origin}/voicelife/reminder-actions/token%2Evalue`, {
            method: 'POST',
            headers: { 'content-type': 'application/x-www-form-urlencoded' },
            body: new globalThis.URLSearchParams({ action: 'snooze', 'params.minutes': '10', token: 'untrusted' }),
        });
        assert.equal(submitted.status, 200);
        assert.equal(await submitted.text(), '<p>submitted</p>');
        assert.deepEqual(actionRequests, [
            { method: 'GET', token: 'token.value' },
            {
                method: 'POST',
                token: 'token.value',
                input: { action: 'snooze', 'params.minutes': '10', token: 'untrusted' },
            },
        ]);

        const queryPage = await globalThis.fetch(`${origin}/voicelife/reminder-actions/query-result/token%2Evalue`);
        assert.equal(queryPage.status, 200);
        assert.equal(await queryPage.text(), '<h1>日程 token.value</h1>');
        assert.deepEqual(scheduleQueryPageRequests, ['token.value']);

        const queryWrite = await globalThis.fetch(`${origin}/voicelife/reminder-actions/query-result/token%2Evalue`, {
            method: 'POST',
        });
        assert.equal(queryWrite.status, 405);
        assert.equal(queryWrite.headers.get('allow'), 'GET');
    });
});

test('WeChat development harness protects test controls and bounds request bodies', async () => {
    await withHarness({}, async ({ origin }) => {
        const unauthorized = await globalThis.fetch(`${origin}/__dev/wechat/send-test`, { method: 'POST' });
        assert.equal(unauthorized.status, 401);

        const wrongDevice = await globalThis.fetch(`${origin}/__dev/wechat/send-test`, {
            method: 'POST',
            headers: { authorization: 'Bearer wrong-device' },
        });
        assert.equal(wrongDevice.status, 401);

        const sent = await globalThis.fetch(`${origin}/__dev/wechat/send-test`, {
            method: 'POST',
            headers: { authorization: `Bearer ${deviceToken}` },
        });
        assert.equal(sent.status, 200);
        assert.deepEqual(await sent.json(), {
            deliveryId: 'delivery-fixture',
            status: 'accepted',
            externalMessageId: '90071992547409931234',
        });

        const inspected = await globalThis.fetch(`${origin}/__dev/wechat/deliveries/delivery-fixture`, {
            headers: { authorization: `Bearer ${deviceToken}` },
        });
        assert.equal(inspected.status, 200);
        assert.deepEqual(await inspected.json(), {
            deliveryId: 'delivery-fixture',
            status: 'delivered',
            externalMessageId: '90071992547409931234',
            attempts: 1,
            receipts: 1,
        });

        const oversized = await globalThis.fetch(`${origin}/wechat`, {
            method: 'POST',
            headers: { 'content-type': 'text/xml' },
            body: 'x'.repeat(64 * 1024 + 1),
        });
        assert.equal(oversized.status, 413);

        const missing = await globalThis.fetch(`${origin}/unknown`);
        assert.equal(missing.status, 404);
        assert.equal(missing.headers.get('cache-control'), 'no-store');
    });
});

test('WeChat development harness rejects unsafe listeners and unsupported request shapes', async () => {
    const required = {
        expectedDeviceId: 'device-fixture',
        authentication: { authenticate: async () => ({ deviceId: 'device-fixture' }) },
        webhookApi: { verify: () => '', post: async () => ({ body: '', contentType: 'text/plain; charset=utf-8' }) },
        actionUiPageApi: {
            get: async () => ({ status: 200, headers: {}, body: '' }),
            post: async () => ({ status: 200, headers: {}, body: '' }),
        },
        scheduleQueryPageApi: { get: async () => ({ status: 200, headers: {}, body: '' }) },
        sendTestNotification: async () => ({ deliveryId: 'delivery', status: 'accepted' }),
        inspectDelivery: async () => undefined,
    };
    await assert.rejects(
        () => startWechatDevHttpHarness({ ...required, host: '0.0.0.0', port: 0 }),
        /loopback address/u,
    );
    await assert.rejects(
        () => startWechatDevHttpHarness({ ...required, host: '127.0.0.1', port: 65_536 }),
        /port is invalid/u,
    );

    await withHarness({ inspectDelivery: async () => undefined }, async ({ origin }) => {
        const webhookWrite = await globalThis.fetch(`${origin}/wechat`, { method: 'PUT' });
        assert.equal(webhookWrite.status, 405);
        assert.equal(webhookWrite.headers.get('allow'), 'GET, POST');

        const unsupportedAction = await globalThis.fetch(`${origin}/voicelife/reminder-actions/token`, {
            method: 'POST',
            body: 'action=acknowledge',
        });
        assert.equal(unsupportedAction.status, 415);

        const actionWrite = await globalThis.fetch(`${origin}/voicelife/reminder-actions/token`, { method: 'PUT' });
        assert.equal(actionWrite.status, 405);
        assert.equal(actionWrite.headers.get('allow'), 'GET, POST');

        const missingDelivery = await globalThis.fetch(`${origin}/__dev/wechat/deliveries/missing`, {
            headers: { authorization: `Bearer ${deviceToken}` },
        });
        assert.equal(missingDelivery.status, 404);

        const malformedToken = await globalThis.fetch(`${origin}/voicelife/reminder-actions/%ZZ`);
        assert.equal(malformedToken.status, 400);
    });
});

test('WeChat development harness returns safe errors from webhook controller failures', async () => {
    await withHarness(
        {
            webhookApi: {
                verify: () => {
                    throw new TypeError('invalid request');
                },
                post: async () => {
                    throw new Error('unexpected failure');
                },
            },
        },
        async ({ origin }) => {
            const invalid = await globalThis.fetch(`${origin}/wechat?signature=s&timestamp=1&nonce=n`);
            assert.equal(invalid.status, 400);
            assert.equal(await invalid.text(), 'Bad Request');

            const unexpected = await globalThis.fetch(`${origin}/wechat?signature=s&timestamp=1&nonce=n`, {
                method: 'POST',
                body: '<xml/>',
            });
            assert.equal(unexpected.status, 500);
            assert.equal(await unexpected.text(), 'Internal Server Error');
        },
    );
});
