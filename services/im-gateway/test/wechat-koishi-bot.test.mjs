import assert from 'node:assert/strict';
import { test } from 'node:test';

import { Context } from '@koishijs/core';

import { KoishiContextBotFacade } from '../dist/infrastructure/koishi/koishi-channel-adapter.js';
import { WechatOfficialKoishiBot } from '../dist/infrastructure/koishi/wechat-official-koishi-bot.js';

test('production WeChat bot registers in Koishi and sends rendered payload through the Context facade', async () => {
    const context = new Context();
    const sent = [];
    const bot = new WechatOfficialKoishiBot(context, {
        koishiBotId: 'wechat:channel-1',
        selfId: 'channel-1',
        transport: {
            sendToUser: async (externalUserId, content) => {
                sent.push({ externalUserId, content });
                return { accepted: true, platformMessageId: 'wechat-message-1' };
            },
        },
    });
    await context.start();

    try {
        assert.equal(bot.isActive, true);
        assert.equal(
            context.bots.some((candidate) => candidate.sid === 'wechat:channel-1'),
            true,
        );
        const result = await new KoishiContextBotFacade(context).sendPrivateMessage({
            platform: 'wechat_official',
            koishiBotId: 'wechat:channel-1',
            platformUserId: 'openid-1',
            content: { type: 'wechat_template', templateId: 'template-1', data: {} },
        });
        assert.deepEqual(result, { platformMessageId: 'wechat-message-1' });
        assert.deepEqual(sent, [
            {
                externalUserId: 'openid-1',
                content: { type: 'wechat_template', templateId: 'template-1', data: {} },
            },
        ]);
    } finally {
        await context.stop();
    }
});

test('production WeChat bot preserves retryable and permanent platform rejection semantics', async () => {
    for (const retryable of [true, false]) {
        const context = new Context();
        new WechatOfficialKoishiBot(context, {
            koishiBotId: `wechat:channel-${String(retryable)}`,
            selfId: `channel-${String(retryable)}`,
            transport: {
                sendToUser: async () => ({ accepted: false, retryable, errorCode: 'wechat_fixture' }),
            },
        });
        await context.start();
        try {
            await assert.rejects(
                () => context.bots[0].sendPrivateMessage('openid-1', JSON.stringify({ type: 'wechat_template' })),
                (error) => error.retryable === retryable && error.errorCode === 'wechat_fixture',
            );
        } finally {
            await context.stop();
        }
    }
});
