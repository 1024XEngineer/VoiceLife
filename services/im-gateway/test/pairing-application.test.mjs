import { test } from 'node:test';
import assert from 'node:assert/strict';

import { buildGateway, expectGatewayError, registerChannel } from './helpers.mjs';

test('create issues a pending ten-minute pairing session by default', async () => {
    const { gateway, clock } = buildGateway();

    const created = await gateway.application.pairing.create({
        userId: 'user-fixture',
        deviceId: 'device-fixture',
    });

    assert.equal(created.displayCode, '123456');
    assert.equal(created.session.userId, 'user-fixture');
    assert.equal(created.session.deviceId, 'device-fixture');
    assert.equal(created.session.status, 'pending');
    assert.equal(created.session.createdAt, clock.now());
    assert.equal(created.session.expiresAt, clock.addMinutes(clock.now(), 10));
    assert.deepEqual(await gateway.application.pairing.find(created.session.id), created.session);
});

test('create preserves platform restrictions and a custom expiry', async () => {
    const { gateway, clock } = buildGateway();

    const created = await gateway.application.pairing.create({
        deviceId: 'device-fixture',
        allowedPlatforms: ['feishu'],
        expiresInMinutes: 3,
    });

    assert.deepEqual(created.session.allowedPlatforms, ['feishu']);
    assert.equal(created.session.userId, undefined);
    assert.equal(created.session.expiresAt, clock.addMinutes(clock.now(), 3));
});

test('confirm protects the external identity and completes the pairing session', async () => {
    const { gateway, clock } = buildGateway();
    const channel = await registerChannel(gateway);
    const created = await gateway.application.pairing.create({
        userId: 'user-fixture',
        deviceId: 'device-fixture',
    });

    const binding = await gateway.application.pairing.confirm({
        displayCode: created.displayCode,
        channelAccountId: channel.id,
        externalUserId: 'open-fixture',
        displayName: 'Fixture User',
    });

    assert.equal(binding.userId, 'user-fixture');
    assert.equal(binding.deviceId, 'device-fixture');
    assert.equal(binding.status, 'active');
    assert.equal(binding.boundAt, clock.now());
    const session = await gateway.application.pairing.find(created.session.id);
    assert.equal(session.status, 'confirmed');
    assert.equal(session.confirmedAt, clock.now());
});

test('anonymous pairing requires and accepts a user during confirmation', async () => {
    const { gateway } = buildGateway();
    const channel = await registerChannel(gateway);
    const missingUser = await gateway.application.pairing.create({ deviceId: 'device-fixture' });

    await expectGatewayError(
        () =>
            gateway.application.pairing.confirm({
                displayCode: missingUser.displayCode,
                channelAccountId: channel.id,
                externalUserId: 'open-fixture',
            }),
        'binding_not_found',
        'Anonymous pairing without a confirmation user was accepted',
    );

    const binding = await gateway.application.pairing.confirm({
        displayCode: missingUser.displayCode,
        channelAccountId: channel.id,
        externalUserId: 'open-fixture',
        userId: 'user-fixture',
    });
    assert.equal(binding.userId, 'user-fixture');
});

test('confirm rejects a user or platform outside the pairing scope', async () => {
    const { gateway } = buildGateway();
    const wechat = await registerChannel(gateway);
    const scopedUser = await gateway.application.pairing.create({
        userId: 'user-fixture',
        deviceId: 'device-fixture',
    });

    await expectGatewayError(
        () =>
            gateway.application.pairing.confirm({
                displayCode: scopedUser.displayCode,
                channelAccountId: wechat.id,
                externalUserId: 'open-fixture',
                userId: 'user-other',
            }),
        'invalid_transition',
        'Pairing confirmation changed the scoped user',
    );

    const { gateway: platformGateway } = buildGateway();
    const platformChannel = await registerChannel(platformGateway);
    const scopedPlatform = await platformGateway.application.pairing.create({
        userId: 'user-fixture',
        deviceId: 'device-fixture',
        allowedPlatforms: ['feishu'],
    });
    await expectGatewayError(
        () =>
            platformGateway.application.pairing.confirm({
                displayCode: scopedPlatform.displayCode,
                channelAccountId: platformChannel.id,
                externalUserId: 'open-fixture',
            }),
        'capability_not_supported',
        'Pairing confirmation ignored allowedPlatforms',
    );
});

test('confirm rejects a disabled or unknown channel account', async () => {
    const { gateway } = buildGateway();
    const channel = await registerChannel(gateway);
    await gateway.application.channels.disable(channel.id);
    const disabled = await gateway.application.pairing.create({
        userId: 'user-fixture',
        deviceId: 'device-fixture',
    });

    for (const channelAccountId of [channel.id, 'channel-missing']) {
        await expectGatewayError(
            () =>
                gateway.application.pairing.confirm({
                    displayCode: disabled.displayCode,
                    channelAccountId,
                    externalUserId: 'open-fixture',
                }),
            'binding_not_found',
            'Pairing confirmation accepted an unavailable channel account',
        );
    }
});

test('cancel and expiry make a pairing code unusable', async () => {
    const { gateway } = buildGateway();
    const channel = await registerChannel(gateway);
    const cancelled = await gateway.application.pairing.create({
        userId: 'user-fixture',
        deviceId: 'device-fixture',
    });
    await gateway.application.pairing.cancel(cancelled.session.id);
    assert.equal((await gateway.application.pairing.find(cancelled.session.id)).status, 'cancelled');
    await expectGatewayError(
        () =>
            gateway.application.pairing.confirm({
                displayCode: cancelled.displayCode,
                channelAccountId: channel.id,
                externalUserId: 'open-cancelled',
            }),
        'binding_not_found',
        'A cancelled pairing code was accepted',
    );

    const { gateway: expiryGateway, clock: expiryClock } = buildGateway();
    const expiryChannel = await registerChannel(expiryGateway);
    const expiring = await expiryGateway.application.pairing.create({
        userId: 'user-fixture',
        deviceId: 'device-fixture',
        expiresInMinutes: 1,
    });
    expiryClock.advanceMinutes(1);
    assert.equal(await expiryGateway.application.pairing.expireDue(), 1);
    assert.equal((await expiryGateway.application.pairing.find(expiring.session.id)).status, 'expired');
    assert.equal(await expiryGateway.application.pairing.expireDue(), 0);
    await expectGatewayError(
        () =>
            expiryGateway.application.pairing.confirm({
                displayCode: expiring.displayCode,
                channelAccountId: expiryChannel.id,
                externalUserId: 'open-expired',
            }),
        'binding_not_found',
        'An expired pairing code was accepted',
    );
});
