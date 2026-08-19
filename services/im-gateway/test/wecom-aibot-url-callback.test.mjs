import assert from 'node:assert/strict';
import { Buffer } from 'node:buffer';
import { createCipheriv, createHash, randomBytes } from 'node:crypto';
import { test } from 'node:test';

import { WecomAibotInboundAdapter, WecomAibotUrlCallbackController } from '../dist/index.js';

const token = 'fixture-wecom-url-token';
const encodingKey = 'abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG';

function encrypt(plain, receiveId = '') {
    const key = Buffer.from(`${encodingKey}=`, 'base64');
    const random = randomBytes(16);
    const body = Buffer.from(plain, 'utf8');
    const receive = Buffer.from(receiveId, 'utf8');
    const payload = Buffer.alloc(16 + 4 + body.length + receive.length);
    random.copy(payload, 0);
    payload.writeUInt32BE(body.length, 16);
    body.copy(payload, 20);
    receive.copy(payload, 20 + body.length);
    const cipher = createCipheriv('aes-256-cbc', key, key.subarray(0, 16));
    return Buffer.concat([cipher.update(payload), cipher.final()]).toString('base64');
}

function signature(timestamp, nonce, encrypted) {
    return createHash('sha1').update([token, timestamp, nonce, encrypted].sort().join('')).digest('hex');
}

function message() {
    return JSON.stringify({
        msgid: 'message-url',
        aibotid: 'bot-fixture',
        from: { userid: 'userid-fixture' },
        chattype: 'single',
        msgtype: 'text',
        text: { content: '绑定 123456' },
        create_time: 1_786_665_600,
    });
}

test('URL callback decrypts an encrypted message and normalizes it', async () => {
    const encrypted = encrypt(message());
    const adapter = new WecomAibotInboundAdapter({
        channelAccountId: 'channel-wecom',
        botId: 'bot-fixture',
        now: () => '2026-08-18T00:00:00.000Z',
    });
    const controller = new WecomAibotUrlCallbackController(adapter, {
        token,
        encodingAesKey: encodingKey,
        postEvent: async (event) => {
            assert.equal(event.type, 'binding.requested');
            assert.equal(event.payload.externalUserId, 'userid-fixture');
        },
    });
    const timestamp = '1786665600';
    const response = await controller.post({
        timestamp,
        nonce: 'nonce-fixture',
        msg_signature: signature(timestamp, 'nonce-fixture', encrypted),
        body: JSON.stringify({ encrypt: encrypted }),
    });
    assert.deepEqual(response, { status: 200, body: 'success' });
});

test('URL callback verifies and decrypts echostr', () => {
    const encrypted = encrypt('url-verification');
    const adapter = new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' });
    const controller = new WecomAibotUrlCallbackController(adapter, {
        token,
        encodingAesKey: encodingKey,
        postEvent: async () => {},
    });
    const timestamp = '1786665600';
    assert.equal(
        controller.verify({
            timestamp,
            nonce: 'nonce-fixture',
            msg_signature: signature(timestamp, 'nonce-fixture', encrypted),
            echostr: encrypted,
        }),
        'url-verification',
    );
});

test('URL callback rejects an invalid signature', async () => {
    const adapter = new WecomAibotInboundAdapter({ channelAccountId: 'channel-wecom', botId: 'bot-fixture' });
    const controller = new WecomAibotUrlCallbackController(adapter, {
        token,
        encodingAesKey: encodingKey,
        postEvent: async () => {},
    });
    const encrypted = encrypt(message());
    await assert.rejects(
        () =>
            controller.post({
                timestamp: '1786665600',
                nonce: 'nonce-fixture',
                msg_signature: 'bad',
                body: JSON.stringify({ encrypt: encrypted }),
            }),
        /signature is invalid/u,
    );
});
