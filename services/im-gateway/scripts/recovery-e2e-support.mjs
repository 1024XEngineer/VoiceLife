import { createHash } from 'node:crypto';

const DELIVERY_EVENTS = ['im.delivery.requested', 'im.delivery.retry-scheduled', 'im.delivery.retry-requested'];

export function deviceFixture(namespace, suffix) {
    return {
        deviceId: `${namespace}_${suffix}`,
        userId: `${namespace}_${suffix}_user`,
        identityId: `${namespace}_${suffix}_identity`,
        bindingId: `${namespace}_${suffix}_binding`,
        token: createHash('sha256').update(`${namespace}:${suffix}:device-token`).digest('base64url'),
    };
}

export function actionTokenSecret(namespace) {
    return `voicelife-recovery-e2e-action-token-secret:${namespace}`;
}

export function publicDevice(device) {
    return { deviceId: device.deviceId, userId: device.userId, token: device.token };
}

export async function seedRecoveryFixtures(unitOfWork, namespace, now, ...devices) {
    const channelId = `${namespace}_channel`;
    await unitOfWork.transaction(async (tx) => {
        if ((await tx.channelAccounts.findById(channelId)) !== undefined) return;
        await tx.channelAccounts.save({
            id: channelId,
            platform: 'wechat_official',
            tenantExternalId: `${channelId}_tenant`,
            koishiBotId: `${channelId}_bot`,
            credentialRef: 'secret://recovery-e2e-generated',
            connectionMode: 'webhook',
            capabilityConfig: { recoveryE2e: true },
            status: 'active',
            createdAt: now,
            updatedAt: now,
        });
        for (const device of devices) {
            await tx.devices.create({
                deviceId: device.deviceId,
                userId: device.userId,
                tokenDigest: createHash('sha256').update(device.token, 'utf8').digest(),
                status: 'active',
                createdAt: now,
                updatedAt: now,
            });
            await tx.identities.createIfAbsent({
                id: device.identityId,
                channelAccountId: channelId,
                externalUserIdCiphertext: `${device.identityId}_ciphertext`,
                externalUserIdHash: `${device.identityId}_hash`,
                displayName: 'recovery-e2e-fixture',
                status: 'active',
                createdAt: now,
                updatedAt: now,
            });
            await tx.bindings.createActiveIfAbsent({
                id: device.bindingId,
                userId: device.userId,
                deviceId: device.deviceId,
                externalIdentityId: device.identityId,
                priority: 10,
                status: 'active',
                boundAt: now,
            });
        }
    });
}

export async function createRecoveryTables(unitOfWork) {
    await unitOfWork.runRaw(`CREATE TABLE IF NOT EXISTS e2e_platform_calls (
        id bigserial PRIMARY KEY,
        delivery_id text NOT NULL,
        business_event_id text NOT NULL,
        outcome text NOT NULL,
        platform_message_id text,
        observed_at timestamptz NOT NULL
    )`);
    await unitOfWork.runRaw(`CREATE TABLE IF NOT EXISTS e2e_platform_sends (
        delivery_id text PRIMARY KEY,
        business_event_id text NOT NULL,
        platform_message_id text NOT NULL,
        action_token text,
        sent_at timestamptz NOT NULL
    )`);
}

export class RecoveryPlatformAdapter {
    constructor(unitOfWork, clock, mode, injectionObserved) {
        this.unitOfWork = unitOfWork;
        this.clock = clock;
        this.mode = mode;
        this.injectionObserved = injectionObserved;
    }

    setMode(mode) {
        this.mode = mode;
    }

    async send(message) {
        const prior = await this.unitOfWork.runRaw(
            'SELECT COUNT(*)::int AS count FROM e2e_platform_calls WHERE delivery_id = $1',
            [message.delivery.id],
        );
        const call = prior[0]?.count ?? 0;
        if (this.mode === 'retry-always' || (this.mode === 'retry-once' && call === 0)) {
            await this.recordCall(message, 'retryable_failed');
            return { accepted: false, retryable: true, errorCode: 'e2e_platform_temporary' };
        }
        if (this.mode === 'permanent') {
            await this.recordCall(message, 'permanent_failed');
            return { accepted: false, retryable: false, errorCode: 'e2e_platform_permanent' };
        }
        const platformMessageId = `e2e-platform-${message.delivery.id}`;
        const actionToken = readActionToken(message.content);
        const inserted = await this.unitOfWork.runRaw(
            `INSERT INTO e2e_platform_sends
                (delivery_id, business_event_id, platform_message_id, action_token, sent_at)
             VALUES ($1, $2, $3, $4, $5)
             ON CONFLICT (delivery_id) DO NOTHING
             RETURNING delivery_id`,
            [
                message.delivery.id,
                message.delivery.businessEventId,
                platformMessageId,
                actionToken ?? null,
                this.clock.now(),
            ],
        );
        await this.recordCall(message, inserted.length === 1 ? 'accepted' : 'idempotent_replay', platformMessageId);
        if (this.mode === 'pause-after-send' && inserted.length === 1) {
            await this.injectionObserved('after_platform_send', message.delivery.id);
            await new Promise(() => {});
        }
        return { accepted: true, platformMessageId };
    }

    recordCall(message, outcome, platformMessageId) {
        return this.unitOfWork.runRaw(
            `INSERT INTO e2e_platform_calls
                (delivery_id, business_event_id, outcome, platform_message_id, observed_at)
             VALUES ($1, $2, $3, $4, $5)`,
            [
                message.delivery.id,
                message.delivery.businessEventId,
                outcome,
                platformMessageId ?? null,
                this.clock.now(),
            ],
        );
    }
}

export async function claimRecoveryEvents(unitOfWork, clock, leaseMinutes = 2) {
    const leaseUntil = new Date(Date.parse(clock.now()) + leaseMinutes * 60_000).toISOString();
    return unitOfWork.transaction((tx) => tx.outbox.claimPending(DELIVERY_EVENTS, clock.now(), leaseUntil, 20));
}

export async function recoverySnapshot(unitOfWork, namespace, eventId) {
    const pattern = eventId === undefined ? `${namespace}%` : eventId;
    const comparison = eventId === undefined ? 'LIKE' : '=';
    const [deliveries, attempts, outbox, actions, platformCalls, platformSends] = await Promise.all([
        unitOfWork.runRaw(
            `SELECT id, business_event_id, correlation_id, status, external_message_id, last_error_code,
                    claimed_at, updated_at
             FROM im_deliveries WHERE business_event_id ${comparison} $1 ORDER BY created_at, id`,
            [pattern],
        ),
        unitOfWork.runRaw(
            `SELECT a.id, a.delivery_id, a.attempt_no, a.status, a.error_code, a.platform_message_id,
                    a.started_at, a.completed_at
             FROM im_delivery_attempts a
             JOIN im_deliveries d ON d.id = a.delivery_id
             WHERE d.business_event_id ${comparison} $1 ORDER BY a.attempt_no, a.id`,
            [pattern],
        ),
        unitOfWork.runRaw(
            `SELECT o.id, o.aggregate_id, o.event_type, o.status, o.attempts, o.available_at, o.published_at
             FROM im_outbox_events o
             JOIN im_deliveries d ON d.id = o.aggregate_id
             WHERE d.business_event_id ${comparison} $1 ORDER BY o.created_at, o.id`,
            [pattern],
        ),
        unitOfWork.runRaw(
            `SELECT a.id, a.operation_id, a.correlation_id, a.delivery_id, a.status, a.result, a.updated_at
             FROM im_actions a
             JOIN im_deliveries d ON d.id = a.delivery_id
             WHERE d.business_event_id ${comparison} $1 ORDER BY a.created_at, a.id`,
            [pattern],
        ),
        unitOfWork.runRaw(
            `SELECT c.delivery_id, c.outcome, c.platform_message_id, c.observed_at
             FROM e2e_platform_calls c
             WHERE c.business_event_id ${comparison} $1 ORDER BY c.id`,
            [pattern],
        ),
        unitOfWork.runRaw(
            `SELECT s.delivery_id, s.platform_message_id, s.action_token, s.sent_at
             FROM e2e_platform_sends s
             WHERE s.business_event_id ${comparison} $1 ORDER BY s.delivery_id`,
            [pattern],
        ),
    ]);
    return {
        deliveries: datesToStrings(deliveries),
        attempts: datesToStrings(attempts),
        outbox: datesToStrings(outbox),
        actions: datesToStrings(actions),
        platformCalls: datesToStrings(platformCalls),
        platformSends: datesToStrings(platformSends),
    };
}

function datesToStrings(rows) {
    return rows.map((row) =>
        Object.fromEntries(
            Object.entries(row).map(([key, value]) => [key, value instanceof Date ? value.toISOString() : value]),
        ),
    );
}

function readActionToken(content) {
    if (typeof content !== 'object' || content === null || Array.isArray(content)) return undefined;
    const actionUi = content.actionUi;
    if (typeof actionUi !== 'object' || actionUi === null || Array.isArray(actionUi)) return undefined;
    return typeof actionUi.token === 'string' ? actionUi.token : undefined;
}
