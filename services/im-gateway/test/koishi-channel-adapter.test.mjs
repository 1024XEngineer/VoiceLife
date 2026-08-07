import assert from 'node:assert/strict';
import { test } from 'node:test';

import { Context } from '@koishijs/core';

import { KoishiChannelAdapter, KoishiContextBotFacade } from '../dist/index.js';

const account = {
    id: 'channel-fixture',
    platform: 'wechat_official',
    tenantExternalId: 'tenant-fixture',
    koishiBotId: 'wechat:bot-fixture',
    credentialRef: 'secret://fixture',
    connectionMode: 'webhook',
    status: 'active',
    createdAt: '2026-08-03T00:00:00.000Z',
    updatedAt: '2026-08-03T00:00:00.000Z',
};

function outboundMessage(overrides = {}) {
    return {
        delivery: {
            id: 'delivery-fixture',
            businessEventId: 'event-fixture',
            correlationId: 'correlation-fixture',
            bindingId: 'binding-fixture',
            channelAccountId: 'channel-fixture',
            kind: 'reminder_due',
            semanticPayload: {},
            presentationType: 'text',
            status: 'sending',
            createdAt: '2026-08-03T00:00:00.000Z',
            updatedAt: '2026-08-03T00:00:00.000Z',
        },
        conversation: {
            channelAccountId: 'channel-fixture',
            externalIdentityId: 'identity-fixture',
            kind: 'direct',
            externalConversationIdCiphertext: 'cipher:openid-fixture',
        },
        content: { type: 'text', text: 'Reminder fixture' },
        ...overrides,
    };
}

function unitOfWork(channelAccount = account) {
    return {
        transaction: async (work) =>
            work({
                channelAccounts: {
                    findById: async (id) => (id === channelAccount?.id ? channelAccount : undefined),
                },
            }),
    };
}

test('Koishi channel adapter resolves the account and sends a direct message through the selected bot', async () => {
    const calls = [];
    const adapter = new KoishiChannelAdapter({
        unitOfWork: unitOfWork(),
        revealExternalUserId: async (ciphertext) => {
            assert.equal(ciphertext, 'cipher:openid-fixture');
            return 'openid-fixture';
        },
        bot: {
            sendPrivateMessage: async (input) => {
                calls.push(input);
                return { platformMessageId: 'koishi-message-fixture' };
            },
        },
    });

    assert.deepEqual(await adapter.send(outboundMessage()), {
        accepted: true,
        platformMessageId: 'koishi-message-fixture',
    });
    assert.deepEqual(calls, [
        {
            koishiBotId: 'wechat:bot-fixture',
            platformUserId: 'openid-fixture',
            content: { type: 'text', text: 'Reminder fixture' },
        },
    ]);
});

test('Koishi channel adapter rejects account, conversation and platform-message boundary violations', async () => {
    let calls = 0;
    const bot = {
        sendPrivateMessage: async () => {
            calls += 1;
            return { platformMessageId: '' };
        },
    };
    const revealExternalUserId = async () => 'openid-fixture';

    const missingAccount = new KoishiChannelAdapter({
        unitOfWork: unitOfWork(null),
        revealExternalUserId,
        bot,
    });
    assert.deepEqual(await missingAccount.send(outboundMessage()), {
        accepted: false,
        retryable: false,
        errorCode: 'koishi_channel_account_unavailable',
    });

    const disabledAccount = new KoishiChannelAdapter({
        unitOfWork: unitOfWork({ ...account, status: 'disabled' }),
        revealExternalUserId,
        bot,
    });
    assert.deepEqual(await disabledAccount.send(outboundMessage()), {
        accepted: false,
        retryable: false,
        errorCode: 'koishi_channel_account_unavailable',
    });

    const adapter = new KoishiChannelAdapter({ unitOfWork: unitOfWork(), revealExternalUserId, bot });
    assert.deepEqual(
        await adapter.send(
            outboundMessage({
                conversation: {
                    ...outboundMessage().conversation,
                    channelAccountId: 'channel-other',
                },
            }),
        ),
        { accepted: false, retryable: false, errorCode: 'koishi_conversation_mismatch' },
    );
    assert.deepEqual(
        await adapter.send(
            outboundMessage({
                conversation: {
                    ...outboundMessage().conversation,
                    kind: 'group',
                },
            }),
        ),
        { accepted: false, retryable: false, errorCode: 'koishi_direct_message_required' },
    );
    assert.deepEqual(await adapter.send(outboundMessage()), {
        accepted: false,
        retryable: true,
        errorCode: 'koishi_missing_message_id',
    });
    assert.equal(calls, 1);
});

test('Koishi channel adapter rejects an empty decrypted recipient without calling the bot', async () => {
    let called = false;
    const adapter = new KoishiChannelAdapter({
        unitOfWork: unitOfWork(),
        revealExternalUserId: async () => '   ',
        bot: {
            sendPrivateMessage: async () => {
                called = true;
                return { platformMessageId: 'unexpected' };
            },
        },
    });

    assert.deepEqual(await adapter.send(outboundMessage()), {
        accepted: false,
        retryable: false,
        errorCode: 'koishi_invalid_recipient',
    });
    assert.equal(called, false);
});

test('Koishi Context facade selects an active real-runtime Bot and maps text payloads', async () => {
    const context = new Context();
    const calls = [];
    const bot = {
        sid: 'wechat:bot-fixture',
        isActive: true,
        sendPrivateMessage: async (...args) => {
            calls.push(args);
            return ['message-from-context'];
        },
    };
    context.bots.push(bot);
    context.bots[bot.sid] = bot;
    const facade = new KoishiContextBotFacade(context);

    assert.deepEqual(
        await facade.sendPrivateMessage({
            koishiBotId: bot.sid,
            platformUserId: 'openid-fixture',
            content: { type: 'text', text: 'Reminder fixture' },
        }),
        { platformMessageId: 'message-from-context' },
    );
    assert.deepEqual(calls, [['openid-fixture', 'Reminder fixture']]);
});

test('Koishi channel adapter classifies a missing runtime Bot as retryable', async () => {
    const adapter = new KoishiChannelAdapter({
        unitOfWork: unitOfWork(),
        revealExternalUserId: async () => 'openid-fixture',
        bot: new KoishiContextBotFacade(new Context()),
    });

    assert.deepEqual(await adapter.send(outboundMessage()), {
        accepted: false,
        retryable: true,
        errorCode: 'koishi_bot_unavailable',
    });
});

test('Koishi channel adapter preserves unexpected Bot transport failures', async () => {
    const expected = new Error('fixture bot transport failure');
    const adapter = new KoishiChannelAdapter({
        unitOfWork: unitOfWork(),
        revealExternalUserId: async () => 'openid-fixture',
        bot: {
            sendPrivateMessage: async () => {
                throw expected;
            },
        },
    });

    await assert.rejects(() => adapter.send(outboundMessage()), expected);
});

test('Koishi Context facade preserves string content and serializes structured fallback content', async () => {
    const context = new Context();
    const contents = [];
    const bot = {
        sid: 'wechat:bot-fixture',
        isActive: true,
        sendPrivateMessage: async (_userId, content) => {
            contents.push(content);
            return ['message-fixture'];
        },
    };
    context.bots[bot.sid] = bot;
    const facade = new KoishiContextBotFacade(context);

    await facade.sendPrivateMessage({
        koishiBotId: bot.sid,
        platformUserId: 'openid-fixture',
        content: 'plain text',
    });
    await facade.sendPrivateMessage({
        koishiBotId: bot.sid,
        platformUserId: 'openid-fixture',
        content: { type: 'card', title: 'Reminder fixture' },
    });

    assert.deepEqual(contents, ['plain text', '{"type":"card","title":"Reminder fixture"}']);
});
