import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ChannelAdapterRegistry, ImGatewayError } from '../dist/index.js';

const capabilities = {
    proactiveMessage: true,
    nativeAction: false,
    actionUi: false,
    deliveryReceipt: false,
    presentationTypes: ['rich_text'],
};

function account(id, platform) {
    return { id, platform, status: 'active' };
}

function adapter(name, calls) {
    return {
        resolve: async (target) => {
            calls.push({ name, operation: 'resolve', target });
            return capabilities;
        },
        render: async (delivery, target, targetCapabilities, context) => {
            calls.push({ name, operation: 'render', delivery, target, targetCapabilities, context });
            return { adapter: name, deliveryId: delivery.id };
        },
        send: async (message) => {
            calls.push({ name, operation: 'send', message });
            return { accepted: true, platformMessageId: `${name}-message` };
        },
    };
}

test('channel adapter registry routes capabilities, rendering, and sending by channel account', async () => {
    const calls = [];
    const registry = new ChannelAdapterRegistry([
        { accountId: 'channel-wechat', adapter: adapter('wechat', calls) },
        { accountId: 'channel-wecom', adapter: adapter('wecom', calls) },
    ]);
    const wechat = account('channel-wechat', 'wechat_official');
    const wecom = account('channel-wecom', 'wecom_aibot');
    const delivery = { id: 'delivery-wecom', channelAccountId: 'channel-wecom' };
    const content = { type: 'markdown', content: '**Reminder**' };

    assert.equal(await registry.resolve(wechat), capabilities);
    assert.deepEqual(await registry.render(delivery, wecom, capabilities, {}), {
        adapter: 'wecom',
        deliveryId: 'delivery-wecom',
    });
    assert.deepEqual(await registry.send({ delivery, conversation: { kind: 'direct' }, content }), {
        accepted: true,
        platformMessageId: 'wecom-message',
    });
    assert.deepEqual(
        calls.map((call) => [call.name, call.operation]),
        [
            ['wechat', 'resolve'],
            ['wecom', 'render'],
            ['wecom', 'send'],
        ],
    );
});

test('channel adapter registry rejects calls for an unregistered channel account', async () => {
    const calls = [];
    const registry = new ChannelAdapterRegistry([{ accountId: 'channel-wechat', adapter: adapter('wechat', calls) }]);
    const unknown = account('channel-unknown', 'wecom_aibot');

    for (const work of [
        () => registry.resolve(unknown),
        () => registry.render({ id: 'delivery-unknown' }, unknown, capabilities, {}),
        () => registry.send({ delivery: { channelAccountId: 'channel-unknown' }, conversation: {}, content: {} }),
    ]) {
        await assert.rejects(work, (error) => error instanceof ImGatewayError && error.code === 'invalid_contract');
    }
    assert.equal(calls.length, 0);
});
