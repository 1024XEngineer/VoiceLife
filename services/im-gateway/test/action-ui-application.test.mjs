import { test } from 'node:test';
import assert from 'node:assert/strict';

import { DefaultActionUiApplication, ImGatewayError } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';

function actionUiFixture() {
    const clock = new FixedClock();
    const calls = [];
    const claims = {
        actionId: 'action-1',
        deliveryId: 'delivery-1',
        expiresAt: clock.addMinutes(clock.now(), 10),
    };
    const tokens = {
        issue: async (issuedClaims) => {
            calls.push(['issue', issuedClaims]);
            return `token:${issuedClaims.actionId}`;
        },
        verify: async (token) => {
            calls.push(['verify', token]);
            if (token === 'bad-token') throw new ImGatewayError('action_not_found', 'bad token');
            return claims;
        },
        fingerprint: async (token) => {
            calls.push(['fingerprint', token]);
            return `hash:${token}`;
        },
    };
    const actions = {
        prepareToken: async (deliveryId) => {
            calls.push(['prepareToken', deliveryId]);
            return { ...claims, deliveryId };
        },
        inspectPrepared: async (preparedClaims) => {
            calls.push(['inspectPrepared', preparedClaims]);
            return {
                state: 'available',
                actionId: preparedClaims.actionId,
                actions: ['acknowledge'],
                options: [{ action: 'acknowledge', label: '知道了' }],
                expiresAt: preparedClaims.expiresAt,
            };
        },
        triggerPrepared: async (command) => {
            calls.push(['triggerPrepared', command]);
            return {
                schemaVersion: '1',
                commandId: command.claims.actionId,
                operationId: 'operation-1',
                correlationId: 'correlation-1',
                deviceId: 'device-fixture',
                actorBindingId: 'binding-1',
                reminderTriggerId: 'trigger-fixture',
                action: command.actionType,
                ...(command.actionParams === undefined ? {} : { params: command.actionParams }),
                occurredAt: clock.now(),
                expiresAt: command.claims.expiresAt,
            };
        },
    };
    return { actionUi: new DefaultActionUiApplication(tokens, actions, clock), clock, calls, claims };
}

test('issue prepares a token and signs the returned claims', async () => {
    const { actionUi, calls } = actionUiFixture();

    const token = await actionUi.issue('delivery-9');

    assert.equal(token, 'token:action-1');
    assert.deepEqual(
        calls.map((call) => call[0]),
        ['prepareToken', 'issue'],
    );
    assert.equal(calls[0][1], 'delivery-9');
    assert.equal(calls[1][1].deliveryId, 'delivery-9');
});

test('show verifies the token and returns the prepared action view', async () => {
    const { actionUi, calls, claims } = actionUiFixture();

    const view = await actionUi.show('token:action-1');

    assert.deepEqual(view.actions, ['acknowledge']);
    assert.deepEqual(
        calls.map((call) => call[0]),
        ['verify', 'inspectPrepared'],
    );
    assert.deepEqual(calls[1][1], claims);
});

test('show rejects an expired token before inspecting the action', async () => {
    const { actionUi, clock, calls } = actionUiFixture();
    clock.advanceMinutes(11);

    await assert.rejects(
        () => actionUi.show('token:action-1'),
        (error) => error.code === 'action_expired',
    );
    assert.deepEqual(
        calls.map((call) => call[0]),
        ['verify'],
    );
});

test('execute verifies, fingerprints and triggers the requested action', async () => {
    const { actionUi, calls } = actionUiFixture();

    const command = await actionUi.execute(
        { token: 'token:action-1', action: 'snooze', params: { minutes: 10 } },
        { actualIdentityId: 'identity-1' },
    );

    assert.equal(command.action, 'snooze');
    assert.deepEqual(command.params, { minutes: 10 });
    assert.deepEqual(
        calls.map((call) => call[0]),
        ['verify', 'fingerprint', 'triggerPrepared'],
    );
    assert.equal(calls[2][1].actionType, 'snooze');
    assert.deepEqual(calls[2][1].actionParams, { minutes: 10 });
    assert.equal(calls[2][1].actionKeyHash, 'hash:token:action-1');
    assert.equal(calls[2][1].actualIdentityId, 'identity-1');
});

test('execute rejects an expired token before fingerprinting', async () => {
    const { actionUi, clock, calls } = actionUiFixture();
    clock.advanceMinutes(11);

    await assert.rejects(
        () => actionUi.execute({ token: 'token:action-1', action: 'acknowledge' }),
        (error) => error.code === 'action_expired',
    );
    assert.deepEqual(
        calls.map((call) => call[0]),
        ['verify'],
    );
});
