import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
    AesGcmExternalIdentityProtector,
    HmacPairingCodePort,
    UuidIdGenerator,
} from '../dist/infrastructure/security/production-ports.js';
import {
    CapabilityChannelHealthPort,
    DirectConversationResolver,
    SystemClock,
} from '../dist/infrastructure/production-support.js';

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
    assert.match(ids.nextPairingSessionId(), /^pairing-[0-9a-f-]{36}$/u);
    assert.match(ids.nextBindingId(), /^binding-[0-9a-f-]{36}$/u);
    assert.match(ids.nextExternalIdentityId(), /^identity-[0-9a-f-]{36}$/u);
    assert.match(ids.nextDeliveryId(), /^delivery-[0-9a-f-]{36}$/u);
    assert.match(ids.nextDeliveryAttemptId(), /^attempt-[0-9a-f-]{36}$/u);
    assert.match(ids.nextDeliveryReceiptId(), /^receipt-[0-9a-f-]{36}$/u);
    assert.match(ids.nextOperationId(), /^operation-[0-9a-f-]{36}$/u);
    assert.match(ids.nextOutboxEventId(), /^outbox-[0-9a-f-]{36}$/u);
    assert.notEqual(ids.nextRequestId(), ids.nextRequestId());
    assert.equal(ids.actionIdForDelivery('delivery-fixture'), 'action-delivery-fixture');
});

test('production clock, channel health and direct conversations expose runtime-safe values', async () => {
    const clock = new SystemClock();
    assert.match(clock.now(), /^\d{4}-\d{2}-\d{2}T/u);
    assert.equal(clock.addMinutes('2026-08-09T00:00:00.000Z', 5), '2026-08-09T00:05:00.000Z');

    const capabilities = { resolve: async () => ({ proactiveMessage: true }) };
    const health = new CapabilityChannelHealthPort(capabilities, clock);
    assert.equal((await health.check({ id: 'channel-1', status: 'active' })).status, 'healthy');
    assert.equal((await health.check({ id: 'channel-1', status: 'disabled' })).status, 'unavailable');
    const degraded = new CapabilityChannelHealthPort({ resolve: async () => ({ proactiveMessage: false }) }, clock);
    assert.equal((await degraded.check({ id: 'channel-1', status: 'active' })).detail, 'proactive_message_unavailable');

    const unavailable = new CapabilityChannelHealthPort(capabilities, clock, async () => false);
    assert.deepEqual(await unavailable.check({ id: 'channel-1', status: 'active' }), {
        accountId: 'channel-1',
        status: 'unavailable',
        checkedAt: clock.now(),
        detail: 'runtime_unavailable',
    });

    const conversation = await new DirectConversationResolver().resolveDirect({
        id: 'identity-1',
        channelAccountId: 'channel-1',
        externalUserIdCiphertext: 'v1.protected',
    });
    assert.deepEqual(conversation, {
        channelAccountId: 'channel-1',
        externalIdentityId: 'identity-1',
        kind: 'direct',
        externalConversationIdCiphertext: 'v1.protected',
    });
});
