import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';

import { ImGatewayError, WechatCapabilityStub, WechatOfficialAdapter, createMockImGateway } from '../dist/index.js';
import { pendingStrongDelivery } from './helpers.mjs';

const token = 'fixture-wechat-token';
const channelAccountId = 'channel-wechat-fixture';
const fixtureRoot = new URL('./fixtures/wechat/', import.meta.url);

function signature(timestamp, nonce) {
    return createHash('sha1').update([token, timestamp, nonce].sort().join('')).digest('hex');
}

function request(xml, overrides = {}) {
    const timestamp = overrides.timestamp ?? '1722643200';
    const nonce = overrides.nonce ?? 'nonce-fixture';
    return {
        signature: overrides.signature ?? signature(timestamp, nonce),
        timestamp,
        nonce,
        body: xml,
    };
}

function adapter(accountId = channelAccountId) {
    return new WechatOfficialAdapter({ channelAccountId: accountId, token });
}

test('rejects a webhook with an invalid signature', async () => {
    await assert.rejects(
        () => adapter().normalizeInbound(request('<xml><MsgType>text</MsgType></xml>', { signature: 'invalid' })),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('rejects an adapter without an account or token', () => {
    assert.throws(
        () => new WechatOfficialAdapter({ channelAccountId: '', token: '' }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('preserves the zero-argument legacy stub contract', async () => {
    const stub = new WechatCapabilityStub();
    await assert.rejects(
        () => stub.normalizeInbound({}),
        (error) => error instanceof ImGatewayError && error.code === 'not_implemented',
    );
});

test('exposes WeChat capabilities and platform-local renderings', async () => {
    const capabilities = await adapter().capabilities({});
    assert.deepEqual(capabilities, {
        proactiveMessage: true,
        nativeAction: false,
        actionUi: true,
        deliveryReceipt: true,
        presentationTypes: ['template', 'text_with_action_ui'],
    });
    assert.deepEqual(await adapter().renderScheduleReceipt({ summary: 'saved' }), { type: 'text', text: 'saved' });
    assert.deepEqual(
        await adapter().renderNotification({ content: { title: 'Reminder' }, reminderTriggerId: 'trigger-1' }),
        { type: 'wechat_template', title: 'Reminder', reminderTriggerId: 'trigger-1' },
    );
});

test('rejects a non-object webhook before attempting verification', async () => {
    await assert.rejects(
        () => adapter().normalizeInbound(null),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('rejects a signed webhook with an invalid event timestamp', async () => {
    const xml = `
      <xml>
        <FromUserName>open_fixture</FromUserName>
        <CreateTime>not-a-timestamp</CreateTime>
        <MsgType>text</MsgType>
        <Content>hello</Content>
        <MsgId>10000</MsgId>
      </xml>`;

    await assert.rejects(
        () => adapter().normalizeInbound(request(xml)),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('rejects malformed XML instead of accepting parser recovery', async () => {
    const xml = `
      <xml>
        <FromUserName>open_fixture</FromUserName>
        <CreateTime>1722643200</CreateTime>
        <MsgType>text</MsgType>
        <Content>hello</xml>`;

    await assert.rejects(
        () => adapter().normalizeInbound(request(xml)),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('returns echostr only after verifying the webhook signature', () => {
    const timestamp = '1722643200';
    const nonce = 'nonce-verification';
    assert.equal(
        adapter().verifyWebhook({
            signature: signature(timestamp, nonce),
            timestamp,
            nonce,
            echostr: 'echo-fixture',
        }),
        'echo-fixture',
    );
});

test('normalizes a text message and derives a stable event id', async () => {
    const xml = `
      <xml>
        <ToUserName><![CDATA[gh_fixture]]></ToUserName>
        <FromUserName><![CDATA[open_fixture]]></FromUserName>
        <CreateTime>1722643200</CreateTime>
        <MsgType><![CDATA[text]]></MsgType>
        <Content><![CDATA[hello]]></Content>
        <MsgId>10001</MsgId>
      </xml>`;
    const first = await adapter().normalizeInbound(request(xml));
    const second = await adapter().normalizeInbound(request(xml));

    assert.equal(first.type, 'message.received');
    assert.equal(first.externalEventId, 'message:10001');
    assert.equal(first.id, second.id);
    assert.deepEqual(first.payload, {
        externalUserId: 'open_fixture',
        messageId: '10001',
        text: 'hello',
    });
    assert.equal(first.occurredAt, '2024-08-03T00:00:00.000Z');
});

test('normalizes media messages and hashes an event without MsgId', async () => {
    const xml = `
      <xml>
        <FromUserName>open_media</FromUserName>
        <CreateTime>1722643200</CreateTime>
        <MsgType>image</MsgType>
      </xml>`;
    const event = await adapter().normalizeInbound(request(xml));

    assert.equal(event.type, 'message.received');
    assert.match(event.externalEventId, /^image:[0-9a-f]{40}$/u);
    assert.deepEqual(event.payload, { externalUserId: 'open_media', messageType: 'image' });
});

test('normalizes a subscribe event without MsgId using its stable event digest', async () => {
    const xml = `
      <xml>
        <FromUserName>open_subscribe</FromUserName>
        <CreateTime>1722643200</CreateTime>
        <MsgType>event</MsgType>
        <Event>subscribe</Event>
      </xml>`;
    const event = await adapter().normalizeInbound(request(xml));

    assert.equal(event.type, 'message.received');
    assert.match(event.externalEventId, /^event:subscribe:[0-9a-f]{40}$/u);
    assert.deepEqual(event.payload, { externalUserId: 'open_subscribe', event: 'subscribed' });
});

test('normalizes a binding code message without leaking WeChat fields', async () => {
    const xml = `
      <xml>
        <FromUserName><![CDATA[open_bind]]></FromUserName>
        <CreateTime>1722643200</CreateTime>
        <MsgType>text</MsgType>
        <Content><![CDATA[绑定 123456]]></Content>
        <MsgId>10002</MsgId>
      </xml>`;
    const event = await adapter().normalizeInbound(request(xml));

    assert.equal(event.type, 'binding.requested');
    assert.deepEqual(event.payload, {
        displayCode: '123456',
        externalUserId: 'open_bind',
    });
});

test('normalizes template delivery callbacks and deduplicates them by platform event', async () => {
    const xml = `
      <xml>
        <FromUserName><![CDATA[gh_fixture]]></FromUserName>
        <CreateTime>1722643260</CreateTime>
        <MsgType><![CDATA[event]]></MsgType>
        <Event><![CDATA[TEMPLATESENDJOBFINISH]]></Event>
        <MsgID>20001</MsgID>
        <Status><![CDATA[success]]></Status>
      </xml>`;
    const first = await adapter().normalizeInbound(request(xml));
    const second = await adapter().normalizeInbound(request(xml));

    assert.equal(first.type, 'delivery.updated');
    assert.equal(first.externalEventId, 'template:20001:success');
    assert.equal(first.id, second.id);
    assert.deepEqual(first.payload, {
        externalEventId: 'template:20001:success',
        channelAccountId,
        externalMessageId: '20001',
        dedupeKey: 'wechat:template:20001:success',
        stage: 'delivered',
        occurredAt: '2024-08-03T00:01:00.000Z',
        platformCode: 'success',
    });
});

test('rejects a template callback without MsgID', async () => {
    const xml = `
      <xml>
        <FromUserName>gh_fixture</FromUserName>
        <CreateTime>1722643260</CreateTime>
        <MsgType>event</MsgType>
        <Event>TEMPLATESENDJOBFINISH</Event>
        <Status>success</Status>
      </xml>`;
    await assert.rejects(
        () => adapter().normalizeInbound(request(xml)),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('accepts query-wrapped webhook metadata and rejects oversized bodies', async () => {
    const xml =
        '<xml><FromUserName>open_fixture</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>hello</Content></xml>';
    const timestamp = '1722643200';
    const nonce = 'query-fixture';
    const valid = {
        query: { signature: signature(timestamp, nonce), timestamp, nonce },
        body: xml,
    };
    assert.equal((await adapter().normalizeInbound(valid)).type, 'message.received');

    await assert.rejects(
        () => adapter().normalizeInbound({ ...request(xml), body: 'x'.repeat(65 * 1024) }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
    await assert.rejects(
        () => adapter().normalizeInbound({ ...request(xml), body: new Uint8Array(65 * 1024) }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('a duplicate delivery webhook fixture creates one receipt and one business effect', async () => {
    const gateway = createMockImGateway('device-fixture');
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    const dispatched = await gateway.application.deliveries.find(deliveryId);
    const xml = await readFile(new URL('template-delivered.xml', fixtureRoot), 'utf8');
    const event = await adapter(dispatched.delivery.channelAccountId).normalizeInbound(request(xml));

    await gateway.application.platformEvents.postEvent(event);
    await gateway.application.platformEvents.postEvent(event);

    const details = await gateway.application.deliveries.find(deliveryId);
    assert.equal(details.delivery.status, 'delivered');
    assert.equal(details.receipts.length, 1);
});
