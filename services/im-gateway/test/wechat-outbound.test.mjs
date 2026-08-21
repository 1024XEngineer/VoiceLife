import { createHash } from 'node:crypto';
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { WechatOfficialAdapter, createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import {
    bindFixtureUser,
    scheduleQueryResultIntent,
    scheduleReceiptIntent,
    strongIntent,
    weakIntent,
} from './helpers.mjs';

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
            queryTemplateId: 'fixture-query-template',
            queryTemplateFields: {
                title: 'first',
                body: 'keyword1',
                time: 'keyword2',
            },
            revealExternalUserId: async (ciphertext) => ciphertext.replace(/^(?:ciphertext|encrypted):/u, ''),
            fetch: fetchImpl,
            ...overrides,
        },
    });
}

function outboundMessage(overrides = {}) {
    return {
        delivery: { id: 'delivery-1' },
        conversation: { externalConversationIdCiphertext: 'ciphertext:fixture-open-id' },
        content: {
            type: 'wechat_template',
            templateId: 'fixture-template',
            data: {
                thing1: { value: 'Reminder' },
                thing2: { value: '' },
                time3: { value: '2026-08-03T00:00:00.000Z' },
            },
        },
        ...overrides,
    };
}

test('configured WeChat outbound advertises template + H5 fallback and renders an opaque action URL', async () => {
    const adapter = outboundAdapter(async () => {
        throw new Error('fetch is not expected while rendering');
    });
    const capabilities = await adapter.capabilities({ id: 'channel-1', platform: 'wechat_official', status: 'active' });
    const mismatchedCapabilities = await adapter.capabilities({
        id: 'channel-other',
        platform: 'wechat_official',
        status: 'active',
    });
    assert.equal(mismatchedCapabilities.proactiveMessage, false);
    assert.equal(
        (await adapter.resolve({ id: 'channel-1', platform: 'wechat_official', status: 'active' })).proactiveMessage,
        true,
    );
    assert.equal(
        (await adapter.resolve({ id: 'channel-1', platform: 'wechat_official', status: 'disabled' })).proactiveMessage,
        false,
    );
    assert.equal(
        (await adapter.resolve({ id: 'channel-1', platform: 'feishu', status: 'active' })).proactiveMessage,
        false,
    );

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
            time3: { value: '2026年8月3日 08:00' },
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
        { queryTemplateId: 'fixture-template' },
        { actionUiBaseUrl: 'http://gateway.example/actions' },
        { templateFields: { title: 'same', body: 'same', time: 'time3' } },
        { queryTemplateFields: { title: 'same', body: 'same', time: 'time3' } },
        { requestTimeoutMs: 0 },
        { requestTimeoutMs: 10_001 },
        { displayTimeZone: 'not/a-time-zone' },
    ]) {
        assert.throws(
            () => outboundAdapter(fetchImpl, override),
            (error) => error.code === 'invalid_contract',
        );
    }
});

test('query template payload passes send validation with its own field mapping', async () => {
    const requests = [];
    const responses = [
        new globalThis.Response(JSON.stringify({ access_token: 'fixture-query-access-token', expires_in: 7200 })),
        new globalThis.Response(JSON.stringify({ errcode: 0, msgid: '123' })),
    ];
    const adapter = outboundAdapter(async (url, init) => {
        requests.push({ url: String(url), init });
        return responses.shift();
    });
    const account = { id: 'channel-1', platform: 'wechat_official', status: 'active' };
    const content = await adapter.render(
        {
            channelAccountId: 'channel-1',
            presentationType: 'template',
            kind: 'schedule_query_result',
            semanticPayload: scheduleQueryResultIntent(),
        },
        account,
        await adapter.capabilities(account),
        { scheduleQueryToken: 'opaque-query-token' },
    );

    assert.deepEqual(await adapter.sendToUser('fixture-open-id', content), {
        accepted: true,
        platformMessageId: '123',
    });
    const body = JSON.parse(requests[1].init.body);
    assert.equal(body.template_id, 'fixture-query-template');
    assert.deepEqual(Object.keys(body.data).sort(), ['first', 'keyword1', 'keyword2']);
    assert.equal(body.url, 'https://gateway.example/voicelife/reminder-actions/query-result/opaque-query-token');
});

test('renders template times in the configured IANA time zone', async () => {
    const adapter = outboundAdapter(
        async () => {
            throw new Error('fetch is not expected while rendering');
        },
        { displayTimeZone: 'America/New_York' },
    );

    const rendered = await adapter.renderNotification(strongIntent(), { actionToken: 'opaque-token' });
    assert.equal(rendered.data.time3.value, '2026年8月2日 20:00');
});

test('WeChat outbound rejects unsafe base URLs and template fields', () => {
    const fetchImpl = async () => new globalThis.Response('{}');
    for (const actionUiBaseUrl of [
        'not-a-url',
        'https://user:password@gateway.example/actions',
        'https://gateway.example/actions?debug=1',
        'https://gateway.example/actions#fragment',
    ]) {
        assert.throws(
            () => outboundAdapter(fetchImpl, { actionUiBaseUrl }),
            (error) => error.code === 'invalid_contract',
        );
    }
    for (const templateFields of [
        { title: '1title', body: 'body', time: 'time' },
        { title: 'title', body: 'body', time: 'time-value' },
    ]) {
        assert.throws(
            () => outboundAdapter(fetchImpl, { templateFields }),
            (error) => error.code === 'invalid_contract',
        );
    }
});

test('WeChat outbound accepts the runtime fetch fallback and rejects a missing fetch implementation', () => {
    const originalFetch = globalThis.fetch;
    try {
        globalThis.fetch = async () => new globalThis.Response('{}');
        assert.doesNotThrow(() => outboundAdapter(undefined, { fetch: undefined }));
        globalThis.fetch = undefined;
        assert.throws(
            () => outboundAdapter(undefined, { fetch: undefined }),
            (error) => error.code === 'invalid_contract',
        );
    } finally {
        globalThis.fetch = originalFetch;
    }
});

test('WeChat outbound validates delivery scope and payload shape before network access', async () => {
    const adapter = outboundAdapter(async () => {
        throw new Error('network must not be called');
    });
    const capabilities = await adapter.capabilities({ id: 'channel-1', platform: 'wechat_official', status: 'active' });
    const account = { id: 'channel-1', platform: 'wechat_official', status: 'active' };
    const validDelivery = {
        channelAccountId: 'channel-1',
        presentationType: 'template',
        kind: 'schedule_receipt',
        semanticPayload: scheduleReceiptIntent(),
    };
    assert.deepEqual(await adapter.render(validDelivery, account, capabilities, {}), {
        type: 'wechat_template',
        templateId: 'fixture-template',
        data: {
            thing1: { value: '日程已更新' },
            thing2: { value: 'created a schedule' },
            time3: { value: '2026年8月3日 08:00' },
        },
    });
    const queryResult = scheduleQueryResultIntent();
    const renderedQueryResult = await adapter.render(
        { ...validDelivery, kind: 'schedule_query_result', semanticPayload: queryResult },
        account,
        capabilities,
        {},
    );
    assert.equal(renderedQueryResult.templateId, 'fixture-query-template');
    assert.equal(renderedQueryResult.data.first.value, '日程查询结果（2 条）');
    assert.equal(
        renderedQueryResult.data.keyword1.value,
        [
            '共 2 条日程',
            '1. 一次性日程',
            '   时间：2026年8月3日 09:00',
            '2. 周期日程',
            '   时间：2026年8月4日 09:00',
            '例外调整：1 条',
            '查询条件：2026-08-03 至 2026-08-10',
            '状态：进行中',
        ].join('\n'),
    );
    assert.equal(renderedQueryResult.data.keyword2.value, '2026年8月3日 08:00');
    for (const delivery of [
        { ...validDelivery, channelAccountId: 'channel-other' },
        { ...validDelivery, presentationType: 'text' },
    ]) {
        await assert.rejects(
            () => adapter.render(delivery, account, capabilities, {}),
            (error) => error.code === 'capability_not_supported',
        );
    }
    for (const content of [
        null,
        { type: 'text', text: 'invalid' },
        { type: 'wechat_template', templateId: '', data: { thing1: { value: 'x' } } },
        { type: 'wechat_template', templateId: 'other-template', data: { thing1: { value: 'x' } } },
        { type: 'wechat_template', templateId: 'fixture-template', data: {} },
        { type: 'wechat_template', templateId: 'fixture-template', data: { '1bad': { value: 'x' } } },
        { type: 'wechat_template', templateId: 'fixture-template', data: { thing1: { value: 1 } } },
        {
            type: 'wechat_template',
            templateId: 'fixture-template',
            data: {
                thing1: { value: 'x' },
                thing2: { value: '' },
                time3: { value: '' },
                unexpected: { value: 'x' },
            },
        },
        {
            type: 'wechat_template',
            templateId: 'fixture-template',
            data: { thing1: { value: 'x' } },
            url: 'http://gateway.example/action',
        },
        {
            type: 'wechat_template',
            templateId: 'fixture-template',
            data: { thing1: { value: 'x' } },
            url: 'https://evil.example/action',
        },
    ]) {
        await assert.rejects(
            () => adapter.send(outboundMessage({ content })),
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
            data: {
                thing1: { value: 'Reminder' },
                thing2: { value: '' },
                time3: { value: '2026-08-03T00:00:00.000Z' },
            },
        },
    });

    assert.deepEqual(acceptance, {
        accepted: false,
        retryable: true,
        errorCode: 'wechat_-1',
    });
    assert.equal(responses.length, 0);
});

test('WeChat outbound classifies invalid recipients and missing platform message ids', async () => {
    const invalidRecipient = outboundAdapter(
        async () => {
            throw new Error('network must not be called');
        },
        {
            revealExternalUserId: async () => '   ',
        },
    );
    assert.deepEqual(await invalidRecipient.send(outboundMessage()), {
        accepted: false,
        retryable: false,
        errorCode: 'wechat_invalid_recipient',
    });

    const missingResponses = [
        new globalThis.Response(JSON.stringify({ access_token: 'fixture' })),
        new globalThis.Response('{}'),
    ];
    const missingMessageId = outboundAdapter(async () => missingResponses.shift());
    assert.deepEqual(await missingMessageId.send(outboundMessage()), {
        accepted: false,
        retryable: true,
        errorCode: 'wechat_missing_msgid',
    });
});

test('WeChat outbound exposes token and response failures without hiding transport errors', async () => {
    const rejectedToken = outboundAdapter(async () => new globalThis.Response(JSON.stringify({ errcode: 45009 })));
    assert.deepEqual(await rejectedToken.send(outboundMessage()), {
        accepted: false,
        retryable: true,
        errorCode: 'wechat_45009',
    });

    const transportFailure = outboundAdapter(async (_url, init) => {
        if (init?.method === 'POST') throw new Error('transport failed');
        return new globalThis.Response(JSON.stringify({ access_token: 'fixture', expires_in: 7200 }));
    });
    await assert.rejects(() => transportFailure.send(outboundMessage()), /transport failed/u);

    const serverFailure = outboundAdapter(async (_url, init) => {
        if (init?.method === 'POST') return new globalThis.Response('{}', { status: 503 });
        return new globalThis.Response(JSON.stringify({ access_token: 'fixture', expires_in: 7200 }));
    });
    assert.deepEqual(await serverFailure.send(outboundMessage()), {
        accepted: false,
        retryable: true,
        errorCode: 'wechat_-1',
    });

    const clientFailure = outboundAdapter(async (_url, init) => {
        if (init?.method === 'POST') return new globalThis.Response('{}', { status: 400 });
        return new globalThis.Response(JSON.stringify({ access_token: 'fixture' }));
    });
    assert.deepEqual(await clientFailure.send(outboundMessage()), {
        accepted: false,
        retryable: false,
        errorCode: 'wechat_400',
    });

    const throttled = outboundAdapter(async (_url, init) => {
        if (init?.method === 'POST') return new globalThis.Response('{}', { status: 429 });
        return new globalThis.Response(JSON.stringify({ access_token: 'fixture' }));
    });
    assert.deepEqual(await throttled.send(outboundMessage()), {
        accepted: false,
        retryable: true,
        errorCode: 'wechat_429',
    });
});
