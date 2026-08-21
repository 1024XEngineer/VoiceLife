import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ImGatewayError, ScheduleQueryPageController, createMockImGateway } from '../dist/index.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { bindFixtureUser, scheduleQueryResultIntent } from './helpers.mjs';

test('read-only schedule query page renders persisted schedule entries without internal payload details', async () => {
    const clock = new FixedClock();
    const gateway = createMockImGateway('device-fixture', clock);
    await bindFixtureUser(gateway);
    const intent = scheduleQueryResultIntent({
        schedules: [
            {
                id: 1,
                event: '产品评审',
                start_time: '2026-08-03 09:00:00',
                end_time: '2026-08-03 10:00:00',
                location: 'A 会议室',
                notes: '带上原型稿',
            },
        ],
        futureOccurrences: [],
        exceptions: [{ id: 3, rule_id: 2, type: 'modify', original_start_time: '2026-08-05 09:00:00' }],
        resultCount: 1,
    });
    const submission = await gateway.application.notifications.submitScheduleQueryResult(intent);
    const token = await gateway.application.scheduleQueryPage.issue(submission.deliveries[0].deliveryId);
    const response = await gateway.scheduleQueryPageApi.get(token);

    assert.equal(response.status, 200);
    assert.match(response.headers['content-security-policy'], /default-src 'none'/u);
    assert.match(response.body, /产品评审/u);
    assert.match(response.body, /8月3日 09:00 - 8月3日 10:00/u);
    assert.match(response.body, /A 会议室/u);
    assert.match(response.body, /带上原型稿/u);
    assert.match(response.body, /包含 1 项例外调整/u);
    assert.doesNotMatch(response.body, /<form|schedule-query-event-fixture|"schedules"/u);
});

test('read-only schedule query page returns safe terminal pages for invalid and expired links', async () => {
    const controller = new ScheduleQueryPageController({
        show: async (token) => {
            if (token === 'expired') throw new ImGatewayError('action_expired', 'expired');
            throw new ImGatewayError('action_not_found', 'missing');
        },
        issue: async () => 'unused',
    });

    const invalid = await controller.get('missing');
    const expired = await controller.get('expired');

    assert.equal(invalid.status, 404);
    assert.match(invalid.body, /查询结果不可用/u);
    assert.doesNotMatch(invalid.body, /missing|action_not_found/u);
    assert.equal(expired.status, 410);
    assert.match(expired.body, /查询链接已过期/u);
    assert.doesNotMatch(expired.body, /expired|action_expired/u);
});
