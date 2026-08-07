import { createHash } from 'node:crypto';
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { WechatOfficialAdapter, createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { bindFixtureUser, scheduleReceiptIntent, strongIntent, weakIntent } from './helpers.mjs';

const webhookToken = 'fixture-webhook-token';
const expectedToUserName = 'gh_fixture';
const nowSeconds = 1_786_089_600;

function signature(timestamp, nonce) {
    return createHash('sha1').update([webhookToken, timestamp, nonce].sort().join('')).digest('hex');
}

function outboundAdapter(fetchImpl, overrides = {}) {
    return new WechatOfficialAdapter({
        channelAccountId: 'channel-1',
        token: webhookToken,
        expectedToUserName,
        now: () => nowSeconds,
        outbound: {
            appId: 'wx-fixture',
            appSecret: 'fixture-app-secret',
            templateId: 'fixture-template',
            actionUiBaseUrl: 'https://gateway.example/voicelife/reminder-actions',
            templateFields: {
                title: 'thing1',
                body: 'thing2',
                time: 'time3',
            },
            revealExternalUserId: async (ciphertext) => ciphertext.replace(/^(?:ciphertext|encrypted):/u, ''),
            fetch: fetchImpl,
            ...overrides,
        },
    });
}

test('configured WeChat outbound advertises template + H5 fallback and renders an opaque action URL', async () => {
    const adapter = outboundAdapter(async () => {
        throw new Error('fetch is not expected while rendering');
    });
    const capabilities = await adapter.capabilities({});

    assert.deepEqual(capabilities, {
        proactiveMessage: true,
        nativeAction: false,
        actionUi: true,
        deliveryReceipt: true,
        presentationTypes: ['template'],
    });
    const rendered = await adapter.renderNotification(strongIntent(), { actionToken: 'opaque/token.value' });
    assert.deepEqual(rendered, {
        type: 'wechat_template',
        templateId: 'fixture-template',
        data: {
            thing1: { value: 'Fixture reminder' },
            thing2: { value: '' },
            time3: { value: '2026-08-03T00:00:00.000Z' },
        },
        url: 'https://gateway.example/voicelife/reminder-actions/opaque%2Ftoken.value',
    });
    assert.equal((await adapter.renderNotification(weakIntent())).url, undefined);
    assert.equal((await adapter.renderScheduleReceipt(scheduleReceiptIntent())).templateId, 'fixture-template');
    await assert.rejects(
        () => adapter.renderNotification(strongIntent()),
        (error) => error.code === 'invalid_contract',
    );
});

test('WeChat outbound rejects invalid deployment configuration before making a request', () => {
    const fetchImpl = async () => new globalThis.Response('{}');
    for (const override of [
        { appId: 'invalid-app' },
        { appSecret: '' },
        { templateId: 'bad template' },
        { actionUiBaseUrl: 'http://gateway.example/actions' },
        { templateFields: { title: 'same', body: 'same', time: 'time3' } },
    ]) {
        assert.throws(
            () => outboundAdapter(fetchImpl, override),
            (error) => error.code === 'invalid_contract',
        );
    }
});

test('real dispatch records platform acceptance separately from the delivered callback', async () => {
    const requests = [];
    const responses = [
        new globalThis.Response(JSON.stringify({ access_token: 'fixture-access-token', expires_in: 7200 })),
        new globalThis.Response('{"errcode":0,"errmsg":"ok","msgid":4625712877545553920}'),
    ];
    const adapter = outboundAdapter(async (url, init) => {
        requests.push({ url: String(url), init });
        return responses.shift();
    });
    const clock = new FixedClock('2026-08-07T10:00:00.000Z');
    const gateway = createMockImGateway('device-fixture', clock, {
        channelCapabilities: adapter,
        deliveryRenderer: adapter,
        imChannel: adapter,
        actionTokens: {
            issue: async () => 'opaque-action-token',
            verify: async () => {
                throw new Error('not used');
            },
            fingerprint: async () => 'not-used',
        },
    });
    await bindFixtureUser(gateway);
    const submission = await gateway.application.notifications.submitNotification(
        strongIntent({
            plannedAt: clock.now(),
            triggerAt: clock.now(),
            occurredAt: clock.now(),
        }),
    );
    const deliveryId = submission.deliveries[0].deliveryId;

    const accepted = await gateway.application.deliveryDispatch.dispatch(deliveryId);

    assert.equal(accepted.status, 'accepted');
    assert.equal(accepted.externalMessageId, '4625712877545553920');
    assert.equal(requests.length, 2);
    assert.match(requests[0].url, /cgi-bin\/token\?/u);
    assert.match(requests[1].url, /message\/template\/send\?access_token=fixture-access-token/u);
    const sentBody = JSON.parse(requests[1].init.body);
    assert.equal(sentBody.touser, 'fixture-open-id');
    assert.equal(sentBody.template_id, 'fixture-template');
    assert.match(sentBody.url, /\/opaque-action-token$/u);

    const timestamp = String(nowSeconds);
    const nonce = 'receipt-fixture';
    const event = await adapter.normalizeInbound({
        signature: signature(timestamp, nonce),
        timestamp,
        nonce,
        body: `<xml><ToUserName>${expectedToUserName}</ToUserName><FromUserName>fixture-open-id</FromUserName><CreateTime>${timestamp}</CreateTime><MsgType>event</MsgType><Event>TEMPLATESENDJOBFINISH</Event><MsgID>4625712877545553920</MsgID><Status>success</Status></xml>`,
    });
    await gateway.application.platformEvents.postEvent(event);

    const delivered = await gateway.application.deliveries.find(deliveryId);
    assert.equal(delivered.delivery.status, 'delivered');
    assert.equal(delivered.attempts[0].status, 'accepted');
    assert.equal(delivered.receipts[0].stage, 'delivered');
});

test('expired access tokens are refreshed once and platform failures keep retry semantics', async () => {
    const responses = [
        new globalThis.Response(JSON.stringify({ access_token: 'stale', expires_in: 7200 })),
        new globalThis.Response(JSON.stringify({ errcode: 42001, errmsg: 'access token expired' })),
        new globalThis.Response(JSON.stringify({ access_token: 'fresh', expires_in: 7200 })),
        new globalThis.Response(JSON.stringify({ errcode: -1, errmsg: 'system busy' })),
    ];
    const adapter = outboundAdapter(async () => responses.shift());
    const acceptance = await adapter.send({
        delivery: { id: 'delivery-1' },
        conversation: { externalConversationIdCiphertext: 'encrypted:fixture-open-id' },
        content: {
            type: 'wechat_template',
            templateId: 'fixture-template',
            data: { thing1: { value: 'Reminder' } },
        },
    });

    assert.deepEqual(acceptance, {
        accepted: false,
        retryable: true,
        errorCode: 'wechat_-1',
    });
    assert.equal(responses.length, 0);
});

test('WeChat access tokens are cached and permanent API rejection is not retried', async () => {
    const requests = [];
    const responses = [
        new globalThis.Response(JSON.stringify({ access_token: 'cached', expires_in: 7200 })),
        new globalThis.Response(JSON.stringify({ errcode: 40003, errmsg: 'invalid openid' })),
        new globalThis.Response(JSON.stringify({ errcode: 40003, errmsg: 'invalid openid' })),
    ];
    const adapter = outboundAdapter(async (url) => {
        requests.push(String(url));
        return responses.shift();
    });
    const message = {
        delivery: { id: 'delivery-1' },
        conversation: { externalConversationIdCiphertext: 'ciphertext:fixture-open-id' },
        content: {
            type: 'wechat_template',
            templateId: 'fixture-template',
            data: { thing1: { value: 'Reminder' } },
        },
    };

    assert.deepEqual(await adapter.send(message), {
        accepted: false,
        retryable: false,
        errorCode: 'wechat_40003',
    });
    assert.deepEqual(await adapter.send(message), {
        accepted: false,
        retryable: false,
        errorCode: 'wechat_40003',
    });
    assert.equal(requests.filter((url) => url.includes('/cgi-bin/token?')).length, 1);
});

test('unconfigured or mismatched WeChat outbound refuses to send without network access', async () => {
    const inboundOnly = new WechatOfficialAdapter({
        channelAccountId: 'channel-1',
        token: webhookToken,
        expectedToUserName,
    });
    assert.deepEqual(await inboundOnly.send({}), {
        accepted: false,
        retryable: false,
        errorCode: 'wechat_not_configured',
    });

    const adapter = outboundAdapter(async () => {
        throw new Error('network must not be called');
    });
    assert.deepEqual(
        await adapter.send({
            delivery: { channelAccountId: 'channel-other' },
            conversation: { externalConversationIdCiphertext: 'ciphertext:fixture-open-id' },
            content: {},
        }),
        { accepted: false, retryable: false, errorCode: 'wechat_account_mismatch' },
    );
    assert.equal((await adapter.resolve({ id: 'channel-other', platform: 'wechat_official' })).proactiveMessage, false);
});

test('WeChat credential rejection is recorded as permanent without attempting template send', async () => {
    const requests = [];
    const adapter = outboundAdapter(async (url) => {
        requests.push(String(url));
        return new globalThis.Response(JSON.stringify({ errcode: 40013, errmsg: 'invalid appid' }));
    });

    assert.deepEqual(
        await adapter.send({
            delivery: { id: 'delivery-1' },
            conversation: { externalConversationIdCiphertext: 'ciphertext:fixture-open-id' },
            content: {
                type: 'wechat_template',
                templateId: 'fixture-template',
                data: { thing1: { value: 'Reminder' } },
            },
        }),
        { accepted: false, retryable: false, errorCode: 'wechat_40013' },
    );
    assert.equal(requests.length, 1);
});
