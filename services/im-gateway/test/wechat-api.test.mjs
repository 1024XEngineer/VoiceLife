import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { test } from 'node:test';

import { WechatOfficialAdapter, WechatWebhookController, createMockImGateway } from '../dist/index.js';
import { ImGatewayError } from '../dist/shared/errors.js';

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

test('replies with the usage guide as passive XML for help without a database write', async () => {
    const posted = [];
    const controller = new WechatWebhookController(adapter(), {
        postEvent: async (event) => {
            posted.push(event);
        },
    });
    const result = await controller.post(
        signedRequest(
            '<xml><ToUserName>gh_controller</ToUserName><FromUserName>open_controller</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>帮助</Content><MsgId>10001</MsgId></xml>',
        ),
    );

    assert.deepEqual(result, {
        contentType: 'application/xml; charset=utf-8',
        body: '<xml><ToUserName><![CDATA[open_controller]]></ToUserName><FromUserName><![CDATA[gh_controller]]></FromUserName><CreateTime>1722643200</CreateTime><MsgType><![CDATA[text]]></MsgType><Content><![CDATA[欢迎使用 VoiceLife。\n发送绑定码：绑定 123456\n输入“帮助”可再次查看说明。]]></Content></xml>',
    });
    assert.equal(posted.length, 0);
});

test('replies with the usage guide for unknown text without a database write', async () => {
    const controller = new WechatWebhookController(adapter(), {
        postEvent: async () => assert.fail('Unknown text must not be persisted before replying'),
    });
    const result = await controller.post(
        signedRequest(
            '<xml><ToUserName>gh_controller</ToUserName><FromUserName>open_controller</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>hello</Content><MsgId>10001</MsgId></xml>',
        ),
    );

    assert.match(result.body, /<!\[CDATA\[欢迎使用 VoiceLife。/u);
    assert.equal(result.contentType, 'application/xml; charset=utf-8');
});

test('replies with a binding confirmation as passive XML', async () => {
    const controller = new WechatWebhookController(adapter(), { postEvent: async () => undefined });
    const result = await controller.post(
        signedRequest(
            '<xml><ToUserName>gh_controller</ToUserName><FromUserName>open_controller</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>绑定 123456</Content><MsgId>10002</MsgId></xml>',
        ),
    );

    assert.deepEqual(result, {
        contentType: 'application/xml; charset=utf-8',
        body: '<xml><ToUserName><![CDATA[open_controller]]></ToUserName><FromUserName><![CDATA[gh_controller]]></FromUserName><CreateTime>1722643200</CreateTime><MsgType><![CDATA[text]]></MsgType><Content><![CDATA[绑定成功。\nVoiceLife 将向你发送提醒消息。\n输入“帮助”可查看使用说明。]]></Content></xml>',
    });
});

test('replies with an actionable message for invalid or expired binding codes', async () => {
    const controller = new WechatWebhookController(adapter(), {
        postEvent: async () => {
            throw new ImGatewayError('pairing_code_invalid', 'Pairing session is invalid');
        },
    });
    const result = await controller.post(
        signedRequest(
            '<xml><ToUserName>gh_controller</ToUserName><FromUserName>open_controller</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>绑定 123456</Content><MsgId>10003</MsgId></xml>',
        ),
    );

    assert.deepEqual(result, {
        contentType: 'application/xml; charset=utf-8',
        body: '<xml><ToUserName><![CDATA[open_controller]]></ToUserName><FromUserName><![CDATA[gh_controller]]></FromUserName><CreateTime>1722643200</CreateTime><MsgType><![CDATA[text]]></MsgType><Content><![CDATA[绑定码无效或已过期，请在设备端重新获取后再试。]]></Content></xml>',
    });
});

test('replies with a specific message when a code belongs to another platform', async () => {
    const controller = new WechatWebhookController(adapter(), {
        postEvent: async () => {
            throw new ImGatewayError('capability_not_supported', 'Platform is not allowed by the pairing session');
        },
    });
    const result = await controller.post(
        signedRequest(
            '<xml><ToUserName>gh_controller</ToUserName><FromUserName>open_controller</FromUserName><CreateTime>1722643200</CreateTime><MsgType>text</MsgType><Content>绑定 123456</Content><MsgId>10004</MsgId></xml>',
        ),
    );

    assert.match(result.body, /此绑定码不适用于当前公众号/u);
});

test('exposes the WeChat webhook controller when the adapter is injected into the composition root', () => {
    const runtime = createMockImGateway('device-controller', undefined, { wechatAdapter: adapter() });
    assert.ok(runtime.wechatApi instanceof WechatWebhookController);
});
