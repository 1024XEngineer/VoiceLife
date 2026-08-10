import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { test } from 'node:test';

import { WechatOfficialAdapter, WechatWebhookController, createMockImGateway } from '../dist/index.js';

const token = 'controller-fixture-token';
const timestamp = '1722643200';
const nonce = 'controller-fixture-nonce';

function signedRequest(body, echostr) {
    return {
        signature: createHash('sha1').update([token, timestamp, nonce].sort().join('')).digest('hex'),
        timestamp,
        nonce,
        ...(echostr === undefined ? {} : { echostr }),
        body,
    };
}

function adapter() {
    return new WechatOfficialAdapter({
        channelAccountId: 'channel-controller',
        expectedToUserName: 'gh_controller',
        token,
        now: () => Number(timestamp),
    });
}

test('verifies the WeChat configuration request through the controller', () => {
    const controller = new WechatWebhookController(adapter(), { postEvent: async () => undefined });
    assert.equal(controller.verify(signedRequest('', 'echo-controller')), 'echo-controller');
});

test('replies with the usage guide as passive XML for help and unknown text', async () => {
    const posted = [];
    const controller = new WechatWebhookController(adapter(), {
        postEvent: async (event) => {
            posted.push(event);
        },
    });
    const result = await controller.post(
        signedRequest(
            '<xml><ToUserName>gh_controller</ToUserName><FromUserName>open_controller</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>hello</Content><MsgId>10001</MsgId></xml>',
        ),
    );

    assert.deepEqual(result, {
        contentType: 'application/xml; charset=utf-8',
        body: '<xml><ToUserName><![CDATA[open_controller]]></ToUserName><FromUserName><![CDATA[gh_controller]]></FromUserName><CreateTime>1722643200</CreateTime><MsgType><![CDATA[text]]></MsgType><Content><![CDATA[欢迎使用 VoiceLife。\n发送绑定码：绑定 ABC123\n输入“帮助”可再次查看说明。]]></Content></xml>',
    });
    assert.equal(posted.length, 1);
    assert.equal(posted[0].type, 'message.received');
});

test('acknowledges binding commands without the fallback help reply', async () => {
    const controller = new WechatWebhookController(adapter(), { postEvent: async () => undefined });
    const result = await controller.post(
        signedRequest(
            '<xml><ToUserName>gh_controller</ToUserName><FromUserName>open_controller</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>绑定 123456</Content><MsgId>10002</MsgId></xml>',
        ),
    );

    assert.deepEqual(result, { body: 'success', contentType: 'text/plain; charset=utf-8' });
});

test('exposes the WeChat webhook controller when the adapter is injected into the composition root', () => {
    const runtime = createMockImGateway('device-controller', undefined, { wechatAdapter: adapter() });
    assert.ok(runtime.wechatApi instanceof WechatWebhookController);
});
