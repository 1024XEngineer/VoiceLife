import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';

import { ImGatewayError, WechatCapabilityStub, WechatOfficialAdapter, createMockImGateway } from '../dist/index.js';
import { pendingStrongDelivery } from './helpers.mjs';

const token = 'fixture-wechat-token';
const channelAccountId = 'channel-wechat-fixture';
const expectedToUserName = 'gh_fixture';
const fixtureNowSeconds = 1722643260;
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
    return new WechatOfficialAdapter({
        channelAccountId: accountId,
        token,
        expectedToUserName,
        now: () => fixtureNowSeconds,
    });
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

test('requires the configured WeChat original account id', () => {
    assert.throws(
        () => new WechatOfficialAdapter({ channelAccountId: 'channel-wechat', token }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('rejects whitespace-only adapter credentials', () => {
    assert.throws(
        () => new WechatOfficialAdapter({ channelAccountId: '   ', token: '   ' }),
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
    const capabilities = await adapter().capabilities({
        id: channelAccountId,
        platform: 'wechat_official',
        status: 'active',
    });
    assert.deepEqual(capabilities, {
        proactiveMessage: false,
        nativeAction: false,
        actionUi: false,
        deliveryReceipt: true,
        presentationTypes: [],
    });
    assert.deepEqual(await adapter().renderScheduleReceipt({ summary: 'saved' }), { type: 'text', text: 'saved' });
    await assert.rejects(
        () => adapter().renderNotification({ content: { title: 'Reminder' }, reminderTriggerId: 'trigger-1' }),
        (error) => error instanceof ImGatewayError && error.code === 'capability_not_supported',
    );
});

test('does not expose an inactive account and declines outbound work when no sender is configured', async () => {
    const wechat = adapter();
    const inactive = { id: 'other-channel', platform: 'wechat_official', status: 'active' };
    const inactiveCapabilities = await wechat.resolve(inactive);
    assert.deepEqual(inactiveCapabilities, {
        proactiveMessage: false,
        nativeAction: false,
        actionUi: false,
        deliveryReceipt: false,
        presentationTypes: [],
    });
    assert.deepEqual(
        await wechat.send({
            delivery: { channelAccountId },
            conversation: { externalConversationIdCiphertext: 'encrypted:open-fixture' },
            content: { type: 'text', text: '提醒' },
        }),
        { accepted: false, retryable: false, errorCode: 'wechat_not_configured' },
    );
    assert.deepEqual(await wechat.sendToUser('open-fixture', { type: 'text', text: '提醒' }), {
        accepted: false,
        retryable: false,
        errorCode: 'wechat_not_configured',
    });
    await assert.rejects(
        () => wechat.render({ presentationType: 'text' }, inactive, inactiveCapabilities, {}),
        (error) => error instanceof ImGatewayError && error.code === 'capability_not_supported',
    );
});

test('configured legacy stub delegates capabilities and rejects malformed webhook wrappers', async () => {
    const stub = new WechatCapabilityStub({
        channelAccountId,
        token,
        expectedToUserName,
        now: () => fixtureNowSeconds,
    });
    assert.deepEqual(
        await stub.capabilities({ id: channelAccountId, platform: 'wechat_official', status: 'active' }),
        await adapter().capabilities({ id: channelAccountId, platform: 'wechat_official', status: 'active' }),
    );
    await assert.rejects(
        () => adapter().normalizeInbound({ ...request('<xml/>'), xml: '<xml/>' }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
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

test('rejects an event timestamp too far in the future', async () => {
    const xml = `<xml><ToUserName>gh_fixture</ToUserName><FromUserName>open_fixture</FromUserName><CreateTime>${fixtureNowSeconds + 301}</CreateTime><MsgType>text</MsgType><Content>hello</Content></xml>`;
    await assert.rejects(
        () => adapter().normalizeInbound(request(xml, { timestamp: String(fixtureNowSeconds) })),
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
    assert.equal(first.id, 'channel-wechat-fixture:wechat:message:10001');
    assert.equal(first.id, second.id);
    assert.deepEqual(first.payload, {
        externalUserId: 'open_fixture',
        messageId: '10001',
        text: 'hello',
    });
    assert.equal(first.occurredAt, '2024-08-03T00:00:00.000Z');
});

test('isolates the normalized event id by channel account', async () => {
    const xml = `
      <xml>
        <ToUserName>gh_fixture</ToUserName>
        <FromUserName>open_fixture</FromUserName>
        <CreateTime>1722643200</CreateTime>
        <MsgType>text</MsgType>
        <Content>hello</Content>
        <MsgId>10001</MsgId>
      </xml>`;
    const first = await adapter('channel-a').normalizeInbound(request(xml));
    const second = await adapter('channel-b').normalizeInbound(request(xml));
    assert.notEqual(first.id, second.id);
});

test('rejects replayed and future webhook timestamps', () => {
    const oldTimestamp = String(fixtureNowSeconds - 301);
    assert.throws(
        () =>
            adapter().verifyWebhook({
                timestamp: oldTimestamp,
                nonce: 'old',
                signature: signature(oldTimestamp, 'old'),
            }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
    assert.throws(
        () =>
            adapter().verifyWebhook({
                timestamp: String(fixtureNowSeconds + 301),
                nonce: 'future',
                signature: signature(String(fixtureNowSeconds + 301), 'future'),
            }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('rejects a signed webhook addressed to a different WeChat account', async () => {
    const xml =
        '<xml><ToUserName>other_account</ToUserName><FromUserName>open_fixture</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>hello</Content></xml>';
    await assert.rejects(
        () => adapter().normalizeInbound(request(xml)),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('rejects unsafe message ids and statuses', async () => {
    const longId = '1'.repeat(65);
    const idXml = `<xml><ToUserName>gh_fixture</ToUserName><FromUserName>open_fixture</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>hello</Content><MsgId>${longId}</MsgId></xml>`;
    await assert.rejects(
        () => adapter().normalizeInbound(request(idXml)),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
    const longStatus = 'x'.repeat(65);
    const statusXml = `<xml><ToUserName>gh_fixture</ToUserName><FromUserName>open_fixture</FromUserName><CreateTime>1722643200</CreateTime><MsgType>event</MsgType><Event>TEMPLATESENDJOBFINISH</Event><MsgID>20001</MsgID><Status>${longStatus}</Status></xml>`;
    await assert.rejects(
        () => adapter().normalizeInbound(request(statusXml)),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
    const eventIdXml = `<xml><ToUserName>gh_fixture</ToUserName><FromUserName>open_fixture</FromUserName><CreateTime>1722643200</CreateTime><MsgType>event</MsgType><Event>subscribe</Event><MsgId>${longId}</MsgId></xml>`;
    await assert.rejects(
        () => adapter().normalizeInbound(request(eventIdXml)),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('rejects invalid UTF-8, entities, encrypted mode, and ambiguous body input', async () => {
    const metadata = { timestamp: String(fixtureNowSeconds), nonce: 'binary' };
    const binary = new Uint8Array([0x3c, 0x78, 0x6d, 0x6c, 0x3e, 0xc3, 0x28, 0x3c, 0x2f, 0x78, 0x6d, 0x6c, 0x3e]);
    await assert.rejects(
        () =>
            adapter().normalizeInbound({
                ...metadata,
                signature: signature(metadata.timestamp, metadata.nonce),
                body: binary,
            }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
    const entityXml = '<!DOCTYPE xml [<!ENTITY x "boom">]><xml><ToUserName>gh_fixture</ToUserName></xml>';
    await assert.rejects(
        () => adapter().normalizeInbound(request(entityXml)),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
    const plainXml =
        '<xml><ToUserName>gh_fixture</ToUserName><FromUserName>open_fixture</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>hello</Content></xml>';
    await assert.rejects(
        () => adapter().normalizeInbound({ ...request(plainXml), encrypt_type: 'aes' }),
        (error) => error instanceof ImGatewayError && error.code === 'capability_not_supported',
    );
    await assert.rejects(
        () => adapter().normalizeInbound({ ...request(plainXml), xml: plainXml }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
    assert.equal(
        (
            await adapter().normalizeInbound({
                query: {
                    signature: [signature(metadata.timestamp, metadata.nonce)],
                    timestamp: [metadata.timestamp],
                    nonce: [metadata.nonce],
                },
                body: plainXml,
            })
        ).type,
        'message.received',
    );
});

test('normalizes media messages and hashes an event without MsgId', async () => {
    const xml = `
      <xml>
        <ToUserName>gh_fixture</ToUserName>
        <FromUserName>open_media</FromUserName>
        <CreateTime>1722643200</CreateTime>
        <MsgType>image</MsgType>
      </xml>`;
    const event = await adapter().normalizeInbound(request(xml));

    assert.equal(event.type, 'message.received');
    assert.match(event.externalEventId, /^image:[0-9a-f]{40}$/u);
    assert.deepEqual(event.payload, { externalUserId: 'open_media', messageType: 'image' });
});

test('normalizes unsupported standard message types for audit without retrying the webhook', async () => {
    const xml = `
      <xml>
        <ToUserName>gh_fixture</ToUserName>
        <FromUserName>open_location</FromUserName>
        <CreateTime>1722643200</CreateTime>
        <MsgType>location</MsgType>
        <MsgId>10003</MsgId>
      </xml>`;
    const event = await adapter().normalizeInbound(request(xml));
    assert.equal(event.type, 'message.received');
    assert.deepEqual(event.payload, {
        externalUserId: 'open_location',
        messageId: '10003',
        messageType: 'location',
    });
});

test('normalizes a subscribe event without MsgId using its stable event digest', async () => {
    const xml = `
      <xml>
        <ToUserName>gh_fixture</ToUserName>
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
        <ToUserName>gh_fixture</ToUserName>
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

test('only accepts the documented six-digit binding code format', async () => {
    for (const content of ['绑定 12345', '绑定 1234567', '绑定 ABC123', '绑定 123456 额外文字']) {
        const xml = `
          <xml>
            <ToUserName>gh_fixture</ToUserName>
            <FromUserName>open_bind</FromUserName>
            <CreateTime>1722643200</CreateTime>
            <MsgType>text</MsgType>
            <Content><![CDATA[${content}]]></Content>
            <MsgId>10003</MsgId>
          </xml>`;
        const event = await adapter().normalizeInbound(request(xml));
        assert.equal(event.type, 'message.received', `Unexpected binding format accepted: ${content}`);
    }
});

test('normalizes template delivery callbacks and deduplicates them by platform event', async () => {
    const xml = `
      <xml>
        <ToUserName><![CDATA[gh_fixture]]></ToUserName>
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
        dedupeKey: 'channel-wechat-fixture:wechat:template:20001:success',
        stage: 'delivered',
        occurredAt: '2024-08-03T00:01:00.000Z',
        platformCode: 'success',
    });
});

test('marks WeChat system delivery failures as retryable', async () => {
    const xml = `
      <xml>
        <ToUserName>gh_fixture</ToUserName>
        <FromUserName>gh_fixture</FromUserName>
        <CreateTime>1722643260</CreateTime>
        <MsgType>event</MsgType>
        <Event>TEMPLATESENDJOBFINISH</Event>
        <MsgID>20002</MsgID>
        <Status>failed: system failed</Status>
      </xml>`;
    const event = await adapter().normalizeInbound(request(xml));
    assert.equal(event.type, 'delivery.updated');
    assert.equal(event.payload.stage, 'failed');
    assert.equal(event.payload.retryable, true);
});

test('rejects a template callback without MsgID', async () => {
    const xml = `
      <xml>
        <ToUserName>gh_fixture</ToUserName>
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
        '<xml><ToUserName>gh_fixture</ToUserName><FromUserName>open_fixture</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>hello</Content></xml>';
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
