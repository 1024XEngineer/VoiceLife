import { Buffer } from 'node:buffer';
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { AesGcmActionTokenPort, ImGatewayError } from '../dist/index.js';

const secret = 'fixture-action-token-secret-with-32-bytes';
const claims = {
    actionId: 'action-internal-fixture',
    deliveryId: 'delivery-internal-fixture',
    expiresAt: '2026-08-07T12:00:00.000Z',
};

test('action tokens are opaque and survive a process restart', async () => {
    const issuer = new AesGcmActionTokenPort(secret, () => Buffer.alloc(12, 7));
    const token = await issuer.issue(claims);

    assert.match(token, /^v1\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$/u);
    assert.doesNotMatch(token, /action-internal|delivery-internal/u);

    const verifier = new AesGcmActionTokenPort(secret);
    assert.deepEqual(await verifier.verify(token), claims);
    assert.equal(await verifier.fingerprint(token), await issuer.fingerprint(token));
});

test('action token verification rejects tampering and another deployment secret', async () => {
    const tokens = new AesGcmActionTokenPort(secret, () => Buffer.alloc(12, 3));
    const token = await tokens.issue(claims);
    const pieces = token.split('.');
    pieces[2] = `${pieces[2].slice(0, -1)}${pieces[2].endsWith('A') ? 'B' : 'A'}`;

    for (const work of [
        () => tokens.verify(pieces.join('.')),
        () => new AesGcmActionTokenPort('another-fixture-secret-with-32-bytes').verify(token),
    ]) {
        await assert.rejects(work, (error) => error instanceof ImGatewayError && error.code === 'action_not_found');
    }
});

test('action token configuration and claims are validated before use', async () => {
    assert.throws(
        () => new AesGcmActionTokenPort('short'),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );

    const tokens = new AesGcmActionTokenPort(secret);
    await assert.rejects(
        () => tokens.issue({ ...claims, expiresAt: 'not-a-time' }),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );
});

test('action token rejects invalid nonce sources and claim bounds', async () => {
    const invalidNonce = new AesGcmActionTokenPort(secret, () => Buffer.alloc(11));
    await assert.rejects(
        () => invalidNonce.issue(claims),
        (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
    );

    const tokens = new AesGcmActionTokenPort(secret);
    for (const invalidClaims of [
        { ...claims, actionId: '' },
        { ...claims, deliveryId: '' },
        { ...claims, actionId: 'a'.repeat(257) },
        { ...claims, deliveryId: 'd'.repeat(257) },
        { ...claims, expiresAt: '2026-08-07T12:00:00Z' },
    ]) {
        await assert.rejects(
            () => tokens.issue(invalidClaims),
            (error) => error instanceof ImGatewayError && error.code === 'invalid_contract',
        );
    }
});

test('action token rejects malformed envelopes and oversized fingerprints', async () => {
    const tokens = new AesGcmActionTokenPort(secret);
    for (const token of [
        'v2.a.b.c',
        'v1.a.b.c',
        'v1.AAAAAAAAAAAAAAAA.AA.AAAAAAAAAAAAAAAAAAAAAA',
        'v1.AAAAAAAAAAAAAAAA..AAAAAAAAAAAAAAAAAAAAAA',
        'v1.AAAAAAAAAAAAAAAA.AA.AA',
    ]) {
        await assert.rejects(
            () => tokens.verify(token),
            (error) => error instanceof ImGatewayError && error.code === 'action_not_found',
        );
    }
    await assert.rejects(
        () => tokens.fingerprint(''),
        (error) => error instanceof ImGatewayError && error.code === 'action_not_found',
    );
    await assert.rejects(
        () => tokens.fingerprint('x'.repeat(4097)),
        (error) => error instanceof ImGatewayError && error.code === 'action_not_found',
    );
});
