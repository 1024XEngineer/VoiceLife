import { test } from 'node:test';
import assert from 'node:assert/strict';

import { createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import { bindFixtureUser, expectGatewayError, seedDevice } from './helpers.mjs';

class ExposedUnitOfWork extends InMemoryImUnitOfWork {
    binding(bindingId) {
        return this.bindingRows.get(bindingId);
    }
    allBindings() {
        return [...this.bindingRows.values()];
    }
}

function bindingGateway() {
    const clock = new FixedClock();
    const uow = new ExposedUnitOfWork();
    const gateway = createMockImGateway('device-fixture', clock, { unitOfWork: uow });
    return { gateway, clock, uow };
}

test('list and identity lookup return only active bindings', async () => {
    const { gateway } = bindingGateway();
    const { binding } = await bindFixtureUser(gateway);

    const listed = await gateway.application.bindings.list('user-fixture');
    const byIdentity = await gateway.application.bindings.findActiveByExternalIdentity(binding.externalIdentityId);

    assert.deepEqual(
        listed.map((item) => item.id),
        [binding.id],
    );
    assert.equal(byIdentity.id, binding.id);
    assert.deepEqual(await gateway.application.bindings.list('user-other'), []);
});

test('repeated pairing reuses the active user, device and external identity binding', async () => {
    const { gateway } = bindingGateway();
    const { channel, binding } = await bindFixtureUser(gateway);
    const pairing = await gateway.application.pairing.create({
        userId: binding.userId,
        deviceId: binding.deviceId,
    });

    const repeated = await gateway.application.pairing.confirm({
        displayCode: pairing.displayCode,
        channelAccountId: channel.id,
        externalUserId: 'fixture-open-id',
    });

    assert.equal(repeated.id, binding.id);
    assert.deepEqual(
        (await gateway.application.bindings.list(binding.userId)).map((item) => item.id),
        [binding.id],
    );
});

test('binding a device to a new identity unbinds its previous identity', async () => {
    const { gateway, uow } = bindingGateway();
    const first = await bindFixtureUser(gateway, { externalUserId: 'open-first' });
    const second = await bindFixtureUser(gateway, { externalUserId: 'open-second' });
    assert.notEqual(first.binding.id, second.binding.id);
    assert.equal(uow.binding(first.binding.id).status, 'unbound');
    assert.equal(uow.binding(second.binding.id).status, 'active');
    assert.equal(uow.allBindings().filter((binding) => binding.status === 'active').length, 1);
});

test('binding an identity to a new device unbinds its previous device', async () => {
    const { gateway, uow } = bindingGateway();
    const first = await bindFixtureUser(gateway, { externalUserId: 'shared-open-id' });
    seedDevice(uow, 'device-second', 'user-fixture', 4);
    const pairing = await gateway.application.pairing.create({ userId: 'user-fixture', deviceId: 'device-second' });
    const secondBinding = await gateway.application.pairing.confirm({
        displayCode: pairing.displayCode,
        channelAccountId: first.channel.id,
        externalUserId: 'shared-open-id',
    });
    const second = { binding: secondBinding };
    assert.equal(uow.binding(first.binding.id).status, 'unbound');
    assert.equal(uow.binding(second.binding.id).status, 'active');
    assert.equal(uow.allBindings().filter((binding) => binding.status === 'active').length, 1);
});

test('unbind records its terminal status and removes the binding from active queries', async () => {
    const { gateway, clock, uow } = bindingGateway();
    const { binding } = await bindFixtureUser(gateway);
    clock.advanceMinutes(1);

    await gateway.application.bindings.unbind(binding.id);

    const stored = uow.binding(binding.id);
    assert.equal(stored.status, 'unbound');
    assert.equal(stored.unboundAt, clock.now());
    assert.deepEqual(await gateway.application.bindings.list(binding.userId), []);
    assert.equal(
        await gateway.application.bindings.findActiveByExternalIdentity(binding.externalIdentityId),
        undefined,
    );
    const unboundAt = stored.unboundAt;
    clock.advanceMinutes(1);
    await gateway.application.bindings.unbind(binding.id);
    assert.equal(uow.binding(binding.id).unboundAt, unboundAt);
    await expectGatewayError(
        () => gateway.application.bindings.revoke(binding.id),
        'invalid_transition',
        'An unbound binding was changed to revoked',
    );
});

test('revoke records its terminal status and removes the binding from active queries', async () => {
    const { gateway, clock, uow } = bindingGateway();
    const { binding } = await bindFixtureUser(gateway);
    clock.advanceMinutes(1);

    await gateway.application.bindings.revoke(binding.id);

    const stored = uow.binding(binding.id);
    assert.equal(stored.status, 'revoked');
    assert.equal(stored.revokedAt, clock.now());
    assert.deepEqual(await gateway.application.bindings.list(binding.userId), []);
    const revokedAt = stored.revokedAt;
    clock.advanceMinutes(1);
    await gateway.application.bindings.revoke(binding.id);
    assert.equal(uow.binding(binding.id).revokedAt, revokedAt);
    await expectGatewayError(
        () => gateway.application.bindings.unbind(binding.id),
        'invalid_transition',
        'A revoked binding was changed to unbound',
    );
});

test('unbind and revoke reject an unknown binding', async () => {
    const { gateway } = bindingGateway();

    for (const change of ['unbind', 'revoke']) {
        await expectGatewayError(
            () => gateway.application.bindings[change]('binding-missing'),
            'binding_not_found',
            `${change} of an unknown binding was not rejected`,
        );
    }
});
