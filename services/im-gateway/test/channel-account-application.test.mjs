import { test } from 'node:test';
import assert from 'node:assert/strict';

import { buildGateway, expectGatewayError, registerChannel } from './helpers.mjs';

test('register persists an active channel account without exposing credentials', async () => {
    const { gateway, clock } = buildGateway();

    const account = await registerChannel(gateway, {
        tenantExternalId: 'tenant-a',
        koishiBotId: 'bot-a',
        credentialRef: 'secret://tenant-a',
        connectionMode: 'both',
    });

    assert.equal(account.platform, 'wechat_official');
    assert.equal(account.tenantExternalId, 'tenant-a');
    assert.equal(account.koishiBotId, 'bot-a');
    assert.equal(account.credentialRef, 'secret://tenant-a');
    assert.equal(account.connectionMode, 'both');
    assert.equal(account.status, 'active');
    assert.equal(account.createdAt, clock.now());
    assert.equal(account.updatedAt, clock.now());
    assert.deepEqual(await gateway.application.channels.find(account.id), account);
});

test('disable makes a channel unavailable and is harmless for an unknown account', async () => {
    const { gateway, clock } = buildGateway();
    const account = await registerChannel(gateway);
    clock.advanceMinutes(1);

    await gateway.application.channels.disable(account.id);
    await gateway.application.channels.disable('channel-missing');

    const disabled = await gateway.application.channels.find(account.id);
    assert.equal(disabled.status, 'disabled');
    assert.equal(disabled.updatedAt, clock.now());
    assert.equal(await gateway.application.channels.find('channel-missing'), undefined);
});

test('health delegates the persisted account to the health port', async () => {
    const checked = [];
    const { gateway, clock } = buildGateway({
        channelHealth: {
            check: async (account) => {
                checked.push(account);
                return {
                    accountId: account.id,
                    status: 'degraded',
                    checkedAt: clock.now(),
                    detail: 'rate limited',
                };
            },
        },
    });
    const account = await registerChannel(gateway);

    const health = await gateway.application.channels.health(account.id);

    assert.equal(checked.length, 1);
    assert.equal(checked[0].id, account.id);
    assert.equal(health.status, 'degraded');
    assert.equal(health.detail, 'rate limited');
});

test('health of an unknown account is rejected before probing the port', async () => {
    let probes = 0;
    const { gateway } = buildGateway({
        channelHealth: {
            check: async () => {
                probes += 1;
                throw new Error('should not probe');
            },
        },
    });

    await expectGatewayError(
        () => gateway.application.channels.health('channel-missing'),
        'binding_not_found',
        'Health probing an unknown account was not rejected',
    );
    assert.equal(probes, 0);
});
