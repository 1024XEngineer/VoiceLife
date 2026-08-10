import { Buffer } from 'node:buffer';
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ActionUiPageController, AesGcmActionTokenPort, ImGatewayError, createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { pendingStrongDelivery } from './helpers.mjs';

function pageFixture(viewOverride) {
    const calls = [];
    const actionUi = {
        show: async (token) => {
            calls.push(['show', token]);
            if (token === 'expired') throw new ImGatewayError('action_expired', 'expired');
            if (token === 'missing') throw new ImGatewayError('action_not_found', 'missing');
            if (viewOverride !== undefined) return viewOverride;
            return {
                state: 'available',
                actionId: 'action-must-not-render',
                actions: ['acknowledge', 'snooze'],
                options: [
                    { action: 'acknowledge', label: '知道了' },
                    { action: 'snooze', label: '推迟 10 分钟', params: { minutes: 10 } },
                ],
                expiresAt: '2026-08-07T12:00:00.000Z',
            };
        },
        execute: async (input) => {
            calls.push(['execute', input]);
            if (input.token === 'explode') throw new Error('unexpected action failure');
            return {
                commandId: 'action-must-not-render',
                operationId: 'operation-must-not-render',
                action: input.action,
                ...(input.params === undefined ? {} : { params: input.params }),
            };
        },
    };
    return { controller: new ActionUiPageController(actionUi), calls };
}

test('H5 page validates the token and renders only server-approved action options', async () => {
    const { controller, calls } = pageFixture();
    const response = await controller.get('opaque.token/<unsafe>');

    assert.equal(response.status, 200);
    assert.equal(response.headers['content-type'], 'text/html; charset=utf-8');
    assert.match(response.headers['content-security-policy'], /default-src 'none'/u);
    assert.equal(response.headers['strict-transport-security'], 'max-age=31536000; includeSubDomains');
    assert.equal(response.headers['permissions-policy'], 'camera=(), microphone=(), geolocation=()');
    assert.match(response.body, /知道了/u);
    assert.match(response.body, /推迟 10 分钟/u);
    assert.match(response.body, /name="action" value="acknowledge"/u);
    assert.match(response.body, /name="params\.minutes" value="10"/u);
    assert.match(response.body, /opaque\.token%2F%3Cunsafe%3E/u);
    assert.doesNotMatch(response.body, /action-must-not-render|delivery-|operation-/u);
    assert.doesNotMatch(response.body, /<script/u);
    assert.deepEqual(calls, [['show', 'opaque.token/<unsafe>']]);
});

test('H5 submission derives the token from the route and is idempotent at the application boundary', async () => {
    const { controller, calls } = pageFixture();

    const first = await controller.post('opaque-token', {
        action: 'snooze',
        params: { minutes: 10 },
        token: 'attacker-controlled-token',
        deliveryId: 'attacker-controlled-delivery',
    });
    const replay = await controller.post('opaque-token', {
        action: 'snooze',
        params: { minutes: 10 },
    });

    assert.equal(first.status, 200);
    assert.equal(replay.status, 200);
    assert.match(first.body, /推迟 10 分钟/u);
    assert.doesNotMatch(first.body, /action-must-not-render|operation-must-not-render/u);
    assert.deepEqual(
        calls.filter(([name]) => name === 'execute'),
        [
            ['execute', { token: 'opaque-token', action: 'snooze', params: { minutes: 10 } }],
            ['execute', { token: 'opaque-token', action: 'snooze', params: { minutes: 10 } }],
        ],
    );
});

test('H5 page returns a safe terminal view for an expired token', async () => {
    const { controller } = pageFixture();
    const response = await controller.get('expired');

    assert.equal(response.status, 410);
    assert.match(response.body, /链接已过期/u);
    assert.doesNotMatch(response.body, /expired|action_expired/u);
});

test('H5 page maps invalid and missing tokens to safe HTTP responses', async () => {
    const { controller } = pageFixture();
    const invalid = await controller.get('');
    assert.equal(invalid.status, 400);
    assert.match(invalid.body, /无法处理/u);
    const missing = await controller.get('missing');
    assert.equal(missing.status, 404);
    assert.match(missing.body, /链接不可用/u);
    assert.doesNotMatch(missing.body, /missing|action_not_found/u);
});

test('H5 submission accepts form-encoded params and keeps unexpected failures visible', async () => {
    const { controller, calls } = pageFixture();
    const nested = await controller.post('opaque-token', { action: 'snooze', 'params.minutes': '10' });
    assert.equal(nested.status, 200);
    assert.deepEqual(calls.at(-1), ['execute', { token: 'opaque-token', action: 'snooze', params: { minutes: 10 } }]);

    const invalid = await controller.post('opaque-token', null);
    assert.equal(invalid.status, 400);
    const unexpected = controller.post('explode', { action: 'acknowledge' });
    await assert.rejects(unexpected, /unexpected action failure/u);
});

test('H5 result page renders the acknowledge terminal branch without internal ids', async () => {
    const { controller } = pageFixture();
    const response = await controller.post('opaque-token', { action: 'acknowledge' });
    assert.equal(response.status, 200);
    assert.match(response.body, /操作已提交，等待设备确认/u);
    assert.doesNotMatch(response.body, /设备已收到确认操作/u);
    assert.doesNotMatch(response.body, /operation-must-not-render|action-must-not-render/u);
});

test('H5 snooze result does not claim the device has already applied the delay', async () => {
    const { controller } = pageFixture();
    const response = await controller.post('opaque-token', { action: 'snooze', params: { minutes: 10 } });
    assert.equal(response.status, 200);
    assert.match(response.body, /已提交推迟 10 分钟的请求，等待设备确认/u);
    assert.doesNotMatch(response.body, /设备会在新时间再次提醒/u);
});

test('H5 refresh renders every consumed action state without reusable controls', async () => {
    const expiresAt = '2026-08-07T12:00:00.000Z';
    const cases = [
        {
            view: { state: 'submitted', action: 'acknowledge', expiresAt },
            expected: /操作已提交，等待设备确认/u,
        },
        {
            view: { state: 'processing', action: 'acknowledge', expiresAt },
            expected: /设备正在处理/u,
        },
        {
            view: { state: 'succeeded', action: 'acknowledge', expiresAt },
            expected: /提醒已处理/u,
        },
        {
            view: { state: 'succeeded', action: 'snooze', params: { minutes: 10 }, expiresAt },
            expected: /设备已确认推迟 10 分钟/u,
        },
        {
            view: { state: 'failed', action: 'acknowledge', expiresAt },
            expected: /操作未完成/u,
        },
        {
            view: { state: 'expired', action: 'acknowledge', expiresAt },
            expected: /操作已过期/u,
        },
    ];

    for (const fixture of cases) {
        const { controller } = pageFixture(fixture.view);
        const response = await controller.get('opaque-token');
        assert.equal(response.status, 200);
        assert.match(response.body, fixture.expected);
        assert.doesNotMatch(response.body, /<form|name="action"|action-must-not-render|operation-/u);
    }
});

test('H5 refresh reflects an action consumed outside the H5 controller', async () => {
    const published = [];
    const clock = new FixedClock();
    const gateway = createMockImGateway('device-fixture', clock, {
        actionTokens: new AesGcmActionTokenPort('fixture-cross-entry-secret-with-32-bytes', () => Buffer.alloc(12, 6)),
        actionStream: {
            publish: async (command) => published.push(command),
            subscribe: async function* () {},
            close: async () => {},
        },
    });
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    const token = await gateway.application.actionUi.issue(deliveryId);
    const claims = await gateway.application.actions.prepareToken(deliveryId);

    await gateway.application.actions.triggerPrepared({
        claims,
        actionType: 'snooze',
        actionParams: { minutes: 10 },
        actionKeyHash: 'cross-entry-action-key',
    });
    const response = await gateway.actionUiPageApi.get(token);

    assert.equal(response.status, 200);
    assert.match(response.body, /已提交推迟 10 分钟的请求，等待设备确认/u);
    assert.doesNotMatch(response.body, /<form|name="action"/u);
    assert.equal(published.length, 1);
});

test('runtime H5 route validates an opaque token and repeated submission dispatches once', async () => {
    const published = [];
    const clock = new FixedClock();
    const gateway = createMockImGateway('device-fixture', clock, {
        actionTokens: new AesGcmActionTokenPort('fixture-action-page-secret-with-32-bytes', () => Buffer.alloc(12, 5)),
        actionStream: {
            publish: async (command) => published.push(command),
            subscribe: async function* () {},
            close: async () => {},
        },
    });
    const deliveryId = await pendingStrongDelivery(gateway);
    await gateway.application.deliveryDispatch.dispatch(deliveryId);
    const token = await gateway.application.actionUi.issue(deliveryId);

    const page = await gateway.actionUiPageApi.get(token);
    const first = await gateway.actionUiPageApi.post(token, { action: 'acknowledge' });
    const submitted = await gateway.actionUiPageApi.get(token);
    const replay = await gateway.actionUiPageApi.post(token, { action: 'acknowledge' });
    const action = await gateway.application.actions.find(published[0].commandId);
    assert.notEqual(action, undefined);
    await gateway.deviceApi.postReminderActionResult({
        authorization: 'Bearer fixture-device-token',
        deviceId: action.deviceId,
        commandId: action.id,
        body: {
            schemaVersion: '1',
            operationId: action.operationId,
            reminderTriggerId: action.reminderTriggerId,
            status: 'succeeded',
            occurredAt: clock.now(),
        },
    });
    const completed = await gateway.actionUiPageApi.get(token);

    assert.equal(page.status, 200);
    assert.equal(first.status, 200);
    assert.equal(submitted.status, 200);
    assert.equal(replay.status, 200);
    assert.equal(completed.status, 200);
    assert.equal(published.length, 1);
    assert.doesNotMatch(page.body, new RegExp(deliveryId, 'u'));
    assert.match(submitted.body, /操作已提交，等待设备确认/u);
    assert.doesNotMatch(submitted.body, /name="action"/u);
    assert.match(completed.body, /提醒已处理/u);
    assert.doesNotMatch(completed.body, /name="action"/u);
});
