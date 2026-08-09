import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
    AesGcmExternalIdentityProtector,
    BearerDeviceAuthenticationPort,
    HmacPairingCodePort,
    UuidIdGenerator,
} from '../dist/infrastructure/security/production-ports.js';

test('production device authentication accepts only the configured device token', async () => {
    const authentication = new BearerDeviceAuthenticationPort(
        'device-fixture',
        'fixture-device-token-with-enough-entropy',
    );

    assert.deepEqual(await authentication.authenticate('Bearer fixture-device-token-with-enough-entropy'), {
        deviceId: 'device-fixture',
    });
    await assert.rejects(authentication.authenticate('Bearer wrong-device-token-with-enough-entropy'));
    await assert.rejects(authentication.authenticate('Basic fixture'));
});

test('production identity protection encrypts at rest and can reveal only valid ciphertext', async () => {
    const identities = new AesGcmExternalIdentityProtector('fixture-identity-secret-with-at-least-32-bytes');
    const protectedIdentity = await identities.protect('wechat-open-id');

    assert.notEqual(protectedIdentity.ciphertext, 'wechat-open-id');
    assert.doesNotMatch(protectedIdentity.ciphertext, /wechat-open-id/u);
    assert.equal(await identities.reveal(protectedIdentity.ciphertext), 'wechat-open-id');
    await assert.rejects(identities.reveal(`${protectedIdentity.ciphertext}tampered`));
});

test('production pairing codes are random, hashed and verifiable without persistence of plaintext', async () => {
    const pairingCodes = new HmacPairingCodePort('fixture-identity-secret-with-at-least-32-bytes');
    const first = await pairingCodes.issue();
    const second = await pairingCodes.issue();

    assert.match(first.displayCode, /^\d{6}$/u);
    assert.notEqual(first.displayCode, second.displayCode);
    assert.notEqual(first.hash, first.displayCode);
    assert.equal(await pairingCodes.hash(first.displayCode), first.hash);
});

test('production id generator emits opaque collision-resistant identifiers', () => {
    const ids = new UuidIdGenerator('wechat-production');

    assert.equal(ids.nextChannelAccountId(), 'wechat-production');
    assert.match(ids.nextDeliveryId(), /^delivery-[0-9a-f-]{36}$/u);
    assert.notEqual(ids.nextRequestId(), ids.nextRequestId());
    assert.equal(ids.actionIdForDelivery('delivery-fixture'), 'action-delivery-fixture');
});
