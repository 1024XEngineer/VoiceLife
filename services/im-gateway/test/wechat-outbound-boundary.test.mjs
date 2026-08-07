import { test } from 'node:test';
import assert from 'node:assert/strict';

import { WechatOfficialAdapter } from '../dist/index.js';

const webhookToken = 'fixture-webhook-token';
const expectedToUserName = 'gh_fixture';

function outboundAdapter(fetchImpl, overrides = {}) {
    return new WechatOfficialAdapter({
        channelAccountId: 'channel-1',
        token: webhookToken,
        expectedToUserName,
        outbound: {
            appId: 'wx-fixture',
            appSecret: 'fixture-app-secret',
            templateId: 'fixture-template',
            actionUiBaseUrl: 'https://gateway.example/voicelife/reminder-actions',
            templateFields: { title: 'thing1', body: 'thing2', time: 'time3' },
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

test('WeChat outbound rejects malformed and oversized platform responses', async () => {
    for (const response of [
        new globalThis.Response('not-json'),
        new globalThis.Response('[]'),
        new globalThis.Response(JSON.stringify({ errcode: 'bad' })),
    ]) {
        const adapter = outboundAdapter(async () => response);
        assert.deepEqual(await adapter.send(outboundMessage()), {
            accepted: false,
            retryable: false,
            errorCode: 'wechat_protocol_error',
        });
    }
    const oversized = outboundAdapter(
        async () => new globalThis.Response(JSON.stringify({ access_token: 'x' }).padEnd(65 * 1024, 'x')),
    );
    assert.deepEqual(await oversized.send(outboundMessage()), {
        accepted: false,
        retryable: false,
        errorCode: 'wechat_protocol_error',
    });

    const oversizedHeader = outboundAdapter(
        async () => new globalThis.Response('{}', { headers: { 'content-length': String(65 * 1024) } }),
    );
    assert.deepEqual(await oversizedHeader.send(outboundMessage()), {
        accepted: false,
        retryable: false,
        errorCode: 'wechat_protocol_error',
    });
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
    const message = outboundMessage();

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

test('WeChat access-token refresh is single-flight for concurrent sends', async () => {
    const requests = [];
    let releaseToken;
    const tokenReady = new Promise((resolve) => {
        releaseToken = resolve;
    });
    const adapter = outboundAdapter(async (url) => {
        const requestUrl = String(url);
        requests.push(requestUrl);
        if (requestUrl.includes('/cgi-bin/token?')) {
            await tokenReady;
            return new globalThis.Response(JSON.stringify({ access_token: 'shared', expires_in: 7200 }));
        }
        return new globalThis.Response(JSON.stringify({ errcode: 40003 }));
    });
    const first = adapter.send(outboundMessage({ delivery: { id: 'delivery-1' } }));
    const second = adapter.send(outboundMessage({ delivery: { id: 'delivery-2' } }));
    await new Promise((resolve) => globalThis.setTimeout(resolve, 5));
    releaseToken();
    await Promise.all([first, second]);
    assert.equal(requests.filter((requestUrl) => requestUrl.includes('/cgi-bin/token?')).length, 1);
});

test('WeChat outbound reads only the top-level platform message id', async () => {
    const responses = [
        new globalThis.Response(JSON.stringify({ access_token: 'fixture', expires_in: 7200 })),
        new globalThis.Response(JSON.stringify({ nested: { msgid: '111' }, msgid: '222' })),
    ];
    const adapter = outboundAdapter(async () => responses.shift());
    assert.deepEqual(await adapter.send(outboundMessage()), {
        accepted: true,
        platformMessageId: '222',
    });
});

test('WeChat outbound aborts a hung request at the configured deadline', async () => {
    const adapter = outboundAdapter(
        async (_url, init) =>
            new Promise((resolve, reject) => {
                const timer = globalThis.setTimeout(() => reject(new Error('mock fetch did not abort')), 40);
                init?.signal?.addEventListener('abort', () => {
                    globalThis.clearTimeout(timer);
                    reject(new globalThis.DOMException('The operation was aborted', 'AbortError'));
                });
            }),
        { requestTimeoutMs: 10 },
    );
    assert.deepEqual(await adapter.send(outboundMessage()), {
        accepted: false,
        retryable: true,
        errorCode: 'wechat_timeout',
    });
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
    await assert.rejects(
        () => inboundOnly.render({}, {}, {}, {}),
        (error) => error.code === 'capability_not_supported',
    );

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
    assert.equal(
        (await adapter.resolve({ id: 'channel-other', platform: 'wechat_official', status: 'active' }))
            .proactiveMessage,
        false,
    );
});

test('WeChat credential rejection is recorded as permanent without attempting template send', async () => {
    const requests = [];
    const adapter = outboundAdapter(async (url) => {
        requests.push(String(url));
        return new globalThis.Response(JSON.stringify({ errcode: 40013, errmsg: 'invalid appid' }));
    });

    assert.deepEqual(await adapter.send(outboundMessage()), {
        accepted: false,
        retryable: false,
        errorCode: 'wechat_40013',
    });
    assert.equal(requests.length, 1);
});
