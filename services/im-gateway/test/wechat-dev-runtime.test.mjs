import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { tokenDigest } from '../dist/application/device-management.js';
import { startConfiguredWechatDevHarness } from '../dist/app/wechat-dev-runtime.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';

const webhookToken = 'fixture-webhook-token';
const deviceToken = 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA';
const platformMessageId = '90071992547409931234';

function fixtureEnvironment(overrides = {}) {
    return {
        WECHAT_DEV_HOST: '127.0.0.1',
        WECHAT_DEV_PORT: '0',
        WECHAT_CHANNEL_ACCOUNT_ID: 'wechat-test',
        WECHAT_APP_ID: 'wx-fixture',
        WECHAT_APP_SECRET: 'fixture-app-secret',
        WECHAT_WEBHOOK_TOKEN: webhookToken,
        WECHAT_EXPECTED_TO_USERNAME: 'gh_fixture',
        WECHAT_TEMPLATE_ID: 'fixture-template',
        WECHAT_TEMPLATE_TITLE_FIELD: 'first',
        WECHAT_TEMPLATE_BODY_FIELD: 'keyword1',
        WECHAT_TEMPLATE_TIME_FIELD: 'keyword2',
        WECHAT_QUERY_TEMPLATE_ID: 'fixture-query-template',
        WECHAT_QUERY_TEMPLATE_TITLE_FIELD: 'first',
        WECHAT_QUERY_TEMPLATE_BODY_FIELD: 'keyword1',
        WECHAT_QUERY_TEMPLATE_TIME_FIELD: 'keyword2',
        WECHAT_ACTION_UI_BASE_URL: 'https://public.example/voicelife/reminder-actions',
        WECHAT_TEST_OPENID: 'fixture-open-id',
        DATABASE_URL: 'postgres://fixture:fixture@localhost/fixture',
        WECHAT_DEV_DEVICE_ID: 'device-fixture',
        ACTION_TOKEN_SECRET: 'fixture-action-token-secret-with-32-bytes',
        WECHAT_WEBHOOK_MODE: 'plain',
        ...overrides,
    };
}

function signature(timestamp, nonce) {
    return createHash('sha1').update([webhookToken, timestamp, nonce].sort().join('')).digest('hex');
}

test('configured WeChat harness sends, receives a real-shaped receipt and serves its Action UI', async () => {
    const platformRequests = [];
    const fetchImpl = async (url, init) => {
        const requestUrl = String(url);
        if (requestUrl.includes('/cgi-bin/token?')) {
            return new globalThis.Response(JSON.stringify({ access_token: 'fixture-access-token', expires_in: 7200 }));
        }
        platformRequests.push({ url: requestUrl, init });
        return new globalThis.Response(`{"errcode":0,"errmsg":"ok","msgid":${platformMessageId}}`);
    };
    const unitOfWork = new InMemoryImUnitOfWork();
    unitOfWork.seedDevice({
        deviceId: 'device-fixture',
        userId: 'user-fixture',
        tokenDigest: tokenDigest(deviceToken),
        status: 'active',
        createdAt: '2026-08-03T00:00:00.000Z',
        updatedAt: '2026-08-03T00:00:00.000Z',
    });
    const harness = await startConfiguredWechatDevHarness(fixtureEnvironment(), { fetch: fetchImpl, unitOfWork });
    try {
        const sent = await globalThis.fetch(`${harness.origin}/__dev/wechat/send-test`, {
            method: 'POST',
            headers: { authorization: `Bearer ${deviceToken}` },
        });
        assert.equal(sent.status, 200);
        const sentSnapshot = await sent.json();
        assert.equal(sentSnapshot.status, 'accepted');
        assert.equal(sentSnapshot.externalMessageId, platformMessageId);

        assert.equal(platformRequests.length, 1);
        const platformBody = JSON.parse(platformRequests[0].init.body);
        assert.equal(platformBody.touser, 'fixture-open-id');
        assert.equal(platformBody.template_id, 'fixture-template');
        assert.match(platformBody.url, /^https:\/\/public\.example\/voicelife\/reminder-actions\/v1\./u);
        assert.deepEqual(Object.keys(platformBody.data), ['first', 'keyword1', 'keyword2']);

        const actionPath = new URL(platformBody.url).pathname;
        const actionPage = await globalThis.fetch(`${harness.origin}${actionPath}`);
        assert.equal(actionPage.status, 200);
        const actionHtml = await actionPage.text();
        assert.match(actionHtml, /知道了/u);
        assert.match(actionHtml, /推迟 10 分钟/u);

        const timestamp = String(Math.floor(Date.now() / 1000));
        const nonce = 'fixture-nonce';
        const receiptXml = `<xml>
<ToUserName><![CDATA[gh_fixture]]></ToUserName>
<FromUserName><![CDATA[fixture-open-id]]></FromUserName>
<CreateTime>${timestamp}</CreateTime>
<MsgType><![CDATA[event]]></MsgType>
<Event><![CDATA[TEMPLATESENDJOBFINISH]]></Event>
<MsgID>${platformMessageId}</MsgID>
<Status><![CDATA[success]]></Status>
</xml>`;
        const receipt = await globalThis.fetch(
            `${harness.origin}/wechat?signature=${signature(timestamp, nonce)}&timestamp=${timestamp}&nonce=${nonce}`,
            { method: 'POST', headers: { 'content-type': 'text/xml' }, body: receiptXml },
        );
        assert.equal(receipt.status, 200);
        assert.equal(await receipt.text(), 'success');

        const inspected = await globalThis.fetch(
            `${harness.origin}/__dev/wechat/deliveries/${sentSnapshot.deliveryId}`,
            { headers: { authorization: `Bearer ${deviceToken}` } },
        );
        assert.equal(inspected.status, 200);
        assert.deepEqual(await inspected.json(), {
            deliveryId: sentSnapshot.deliveryId,
            status: 'delivered',
            externalMessageId: platformMessageId,
            attempts: 1,
            receipts: 1,
        });
    } finally {
        await harness.close();
    }
});

test('configured WeChat harness closes its owned database resource when migration fails', async () => {
    let closes = 0;
    const unitOfWork = new InMemoryImUnitOfWork();
    unitOfWork.migrate = async () => {
        throw new Error('migration failed');
    };
    unitOfWork.close = async () => {
        closes += 1;
    };
    await assert.rejects(
        startConfiguredWechatDevHarness(fixtureEnvironment(), {
            unitOfWorkFactory: () => unitOfWork,
        }),
        /migration failed/u,
    );
    assert.equal(closes, 1);
});

test('configured WeChat harness rejects incomplete or unsupported deployment settings', async () => {
    await assert.rejects(
        startConfiguredWechatDevHarness(fixtureEnvironment({ WECHAT_APP_SECRET: '' })),
        /WECHAT_APP_SECRET/u,
    );
    await assert.rejects(
        startConfiguredWechatDevHarness(fixtureEnvironment({ WECHAT_WEBHOOK_MODE: 'aes' })),
        /WECHAT_WEBHOOK_MODE/u,
    );
    await assert.rejects(
        startConfiguredWechatDevHarness(
            fixtureEnvironment({ WECHAT_EXPECTED_TO_USERNAME: 'Test Account Display Name' }),
        ),
        /WECHAT_EXPECTED_TO_USERNAME/u,
    );
});
