import { createHash, randomBytes } from 'node:crypto';

import { createKoishiGatewayRuntime } from '../dist/app/create-koishi-gateway.js';
import { DeliveryOutboxWorker } from '../dist/infrastructure/delivery-outbox-worker.js';
import { startGatewayHttpServer } from '../dist/infrastructure/http/gateway-http-server.js';
import {
    FixedClock,
    InMemoryActionTokenPort,
    MockChannelCapabilities,
    MockDeliveryRenderer,
    MockPairingCodePort,
} from '../dist/infrastructure/mock-support.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';
import { DirectConversationResolver } from '../dist/infrastructure/production-support.js';
import { DatabaseDeviceAuthenticationPort, UuidIdGenerator } from '../dist/infrastructure/security/production-ports.js';

const databaseUrl = process.env.E2E_DATABASE_URL;
const namespace = process.env.E2E_PREFIX;

async function start(connectionString, prefix) {
    const target = deviceFixture(prefix, 'device');
    const isolation = deviceFixture(prefix, 'isolation_device');
    const channelId = `${prefix}_channel`;
    const now = new Date().toISOString();
    const clock = new FixedClock(now);
    const logs = [];
    const unitOfWork = new PostgresImUnitOfWork(connectionString);
    const workerUnitOfWorks = [new PostgresImUnitOfWork(connectionString), new PostgresImUnitOfWork(connectionString)];
    const platform = new ObservablePlatformAdapter(prefix, channelId, clock);
    let gateway;
    let server;
    let workers;
    let closing;
    let terminating = false;

    const close = async () => {
        if (closing !== undefined) return closing;
        closing = runCleanup([
            () => server?.close(),
            () => workers?.close(),
            () => gateway?.close(),
            ...workerUnitOfWorks.map((workerUnitOfWork) => () => workerUnitOfWork.close()),
            () => unitOfWork.close(),
        ]);
        return closing;
    };

    const terminate = async (requestedExitCode) => {
        if (terminating) return;
        terminating = true;
        let exitCode = requestedExitCode;
        try {
            await close();
        } catch {
            exitCode = 1;
        }
        process.exit(exitCode);
    };

    process.once('disconnect', () => void terminate(process.exitCode ?? 0));
    process.once('SIGTERM', () => void terminate(process.exitCode ?? 0));

    try {
        await unitOfWork.migrate();
        await seed(unitOfWork, channelId, now, target, isolation);
        gateway = createKoishiGatewayRuntime({
            dependencies: {
                unitOfWork,
                actionTokens: new InMemoryActionTokenPort(),
                authentication: new DatabaseDeviceAuthenticationPort(unitOfWork),
                channelCapabilities: new MockChannelCapabilities(),
                channelHealth: {
                    check: async (account) => ({ accountId: account.id, status: 'healthy', checkedAt: clock.now() }),
                },
                conversations: new DirectConversationResolver(),
                deliveryRenderer: new MockDeliveryRenderer(),
                imChannel: platform,
                pairingCodes: new MockPairingCodePort(),
                identityProtector: {
                    protect: async (value) => ({ ciphertext: `protected:${value}`, hash: `hash:${value}` }),
                },
                clock,
                ids: new UuidIdGenerator(channelId),
            },
            capabilities: [],
            revealExternalUserId: async (value) => value,
        });
        platform.attachReceipts(gateway.runtime.application.receipts);
        await gateway.start();
        workers = new ConcurrentWorkerPair(
            workerUnitOfWorks.map(
                (workerUnitOfWork) =>
                    new DeliveryOutboxWorker({
                        unitOfWork: workerUnitOfWork,
                        dispatch: gateway.runtime.application.deliveryDispatch,
                        clock,
                        logger: { log: (entry) => logs.push(entry) },
                        pollIntervalMs: 25,
                    }),
            ),
        );
        server = await startGatewayHttpServer({
            host: '127.0.0.1',
            port: 0,
            runtime: gateway.runtime,
            logger: { log: (entry) => logs.push(entry) },
            deliveryAvailable: () => workers.wake(),
            healthCheck: async () => {
                await unitOfWork.runRaw('SELECT 1');
                return { status: 'ok' };
            },
        });
        process.on(
            'message',
            (message) =>
                void handleMessage(
                    message,
                    unitOfWork,
                    workers,
                    platform,
                    logs,
                    [target.token, isolation.token],
                    close,
                ),
        );
        await send({
            type: 'ready',
            origin: server.origin,
            target: publicDevice(target),
            isolation: publicDevice(isolation),
        });
    } catch (error) {
        await send({
            type: 'failed',
            code: infrastructureFailure(error) ? 'infrastructure_failed' : 'gateway_start_failed',
        });
        await terminate(1);
    }
}

async function seed(unitOfWork, channelId, now, ...devices) {
    await unitOfWork.transaction(async (tx) => {
        await tx.channelAccounts.save({
            id: channelId,
            platform: 'wechat_official',
            tenantExternalId: `${channelId}_tenant`,
            koishiBotId: `${channelId}_bot`,
            credentialRef: 'secret://host-e2e-generated',
            connectionMode: 'webhook',
            capabilityConfig: { e2e: true },
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
                displayName: 'host-e2e-fixture',
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

async function handleMessage(message, unitOfWork, workers, platform, logs, deviceTokens, close) {
    if (typeof message !== 'object' || message === null || typeof message.requestId !== 'number') return;
    try {
        if (message.type === 'snapshot') {
            await workers.idle();
            const snapshot = await platform.snapshot(unitOfWork);
            const serializedLogs = JSON.stringify(logs);
            const sensitiveLogLeak =
                deviceTokens.some((token) => serializedLogs.includes(token)) ||
                platform.actionTokens().some((token) => serializedLogs.includes(token));
            await respond(message.requestId, { ...snapshot, sensitiveLogLeak, dispatchRaces: workers.snapshot() });
            return;
        }
        if (message.type === 'deliver') {
            await platform.deliverTarget();
            await respond(message.requestId, { delivered: true });
            return;
        }
        if (message.type === 'stop') {
            await close();
            await respond(message.requestId, { stopped: true });
            process.disconnect();
            return;
        }
        await respond(message.requestId, undefined, 'unknown_process_command');
    } catch {
        await respond(
            message.requestId,
            undefined,
            message.type === 'stop' ? 'cleanup_failed' : 'process_command_failed',
        );
        if (message.type === 'stop') {
            process.exitCode = 1;
            process.disconnect();
        }
    }
}

class ConcurrentWorkerPair {
    constructor(workers) {
        this.workers = workers;
        this.pending = false;
        this.running = undefined;
        this.failure = undefined;
        this.closed = false;
        this.races = [];
    }

    wake() {
        if (this.closed) return;
        this.pending = true;
        this.startDrain();
    }

    async idle() {
        while (this.running !== undefined) await this.running;
        if (this.failure !== undefined) throw this.failure;
    }

    snapshot() {
        return this.races.map((race) => [...race]);
    }

    async close() {
        this.closed = true;
        await this.idle();
        await runCleanup(this.workers.map((worker) => () => worker.close()));
    }

    startDrain() {
        if (this.running !== undefined || this.failure !== undefined) return;
        const running = this.drain();
        this.running = running;
        void running
            .catch((error) => {
                this.failure = error;
            })
            .finally(() => {
                if (this.running === running) this.running = undefined;
                if (this.pending && !this.closed) this.startDrain();
            });
    }

    async drain() {
        while (this.pending && !this.closed) {
            this.pending = false;
            this.races.push(await runWorkerRace(this.workers));
        }
    }
}

async function runWorkerRace(workers) {
    let releaseBarrier = () => undefined;
    const barrier = new Promise((resolve) => {
        releaseBarrier = resolve;
    });
    let waiting = 0;
    return Promise.all(
        workers.map(async (worker) => {
            waiting += 1;
            if (waiting === workers.length) releaseBarrier();
            await barrier;
            return worker.runOnce();
        }),
    );
}

class ObservablePlatformAdapter {
    constructor(namespace, channelId, clock) {
        this.namespace = namespace;
        this.channelId = channelId;
        this.clock = clock;
        this.sends = [];
        this.receipts = undefined;
    }

    attachReceipts(receipts) {
        this.receipts = receipts;
    }

    send(message) {
        const platformMessageId = `platform-${this.sends.length + 1}`;
        const actionToken = readActionToken(message.content);
        this.sends.push({
            platformMessageId,
            businessEventId: message.delivery.businessEventId,
            ...(actionToken === undefined ? {} : { actionToken }),
        });
        return Promise.resolve({ accepted: true, platformMessageId });
    }

    async deliverTarget() {
        const target = this.sends.find((item) => item.businessEventId === `${this.namespace}_strong`);
        if (target === undefined || this.receipts === undefined) throw new Error('target_send_not_ready');
        await this.receipts.record({
            externalEventId: `${this.namespace}_receipt`,
            channelAccountId: this.channelId,
            externalMessageId: target.platformMessageId,
            dedupeKey: `${this.namespace}_receipt_dedupe`,
            stage: 'delivered',
            occurredAt: this.clock.now(),
        });
    }

    actionTokens() {
        return this.sends.flatMap((item) => (item.actionToken === undefined ? [] : [item.actionToken]));
    }

    async snapshot(unitOfWork) {
        const counts = await unitOfWork.runRaw(
            `SELECT
                (SELECT COUNT(*)::int FROM im_deliveries WHERE business_event_id LIKE $1) AS deliveries,
                (SELECT COUNT(*)::int FROM im_delivery_attempts
                    WHERE delivery_id IN (SELECT id FROM im_deliveries WHERE business_event_id LIKE $1)) AS attempts,
                (SELECT COUNT(*)::int FROM im_delivery_receipts
                    WHERE delivery_id IN (SELECT id FROM im_deliveries WHERE business_event_id LIKE $1)) AS receipts,
                (SELECT COUNT(*)::int FROM im_actions WHERE device_id LIKE $1) AS actions,
                (SELECT status FROM im_deliveries WHERE business_event_id = $2 LIMIT 1) AS target_status`,
            [`${this.namespace}%`, `${this.namespace}_strong`],
        );
        const actions = await unitOfWork.runRaw(
            `SELECT device_id, status, result, updated_at
             FROM im_actions
             WHERE device_id LIKE $1
             ORDER BY device_id`,
            [`${this.namespace}%`],
        );
        return {
            deliveryCount: counts[0]?.deliveries ?? 0,
            attemptCount: counts[0]?.attempts ?? 0,
            receiptCount: counts[0]?.receipts ?? 0,
            actionCount: counts[0]?.actions ?? 0,
            targetStatus: counts[0]?.target_status,
            sendCount: this.sends.length,
            weakHasActionToken:
                this.sends.find((item) => item.businessEventId === `${this.namespace}_weak`)?.actionToken !== undefined,
            targetActionToken: this.sends.find((item) => item.businessEventId === `${this.namespace}_strong`)
                ?.actionToken,
            isolationActionToken: this.sends.find(
                (item) => item.businessEventId === `${this.namespace}_isolation_strong`,
            )?.actionToken,
            actions: actions.map((action) => ({
                deviceId: action.device_id,
                status: action.status,
                result: action.result,
                updatedAt:
                    action.updated_at instanceof Date ? action.updated_at.toISOString() : String(action.updated_at),
            })),
        };
    }
}

function deviceFixture(namespace, suffix) {
    return {
        deviceId: `${namespace}_${suffix}`,
        userId: `${namespace}_${suffix}_user`,
        identityId: `${namespace}_${suffix}_identity`,
        bindingId: `${namespace}_${suffix}_binding`,
        token: randomBytes(32).toString('base64url'),
    };
}

function publicDevice(device) {
    return { deviceId: device.deviceId, userId: device.userId, token: device.token };
}

function readActionToken(content) {
    if (typeof content !== 'object' || content === null || Array.isArray(content)) return undefined;
    const actionUi = content.actionUi;
    if (typeof actionUi !== 'object' || actionUi === null || Array.isArray(actionUi)) return undefined;
    return typeof actionUi.token === 'string' ? actionUi.token : undefined;
}

async function runCleanup(callbacks) {
    const errors = [];
    for (const callback of callbacks) {
        try {
            await callback();
        } catch (error) {
            errors.push(error);
        }
    }
    if (errors.length > 0) throw new AggregateError(errors, 'host_e2e_gateway_cleanup_failed');
}

async function respond(requestId, data, error) {
    await send({
        type: 'response',
        requestId,
        ...(error === undefined ? { data } : { error }),
    });
}

function send(message) {
    return new Promise((resolve) => {
        if (!process.connected) {
            resolve();
            return;
        }
        process.send(message, () => resolve());
    });
}

function infrastructureFailure(error) {
    return (
        error !== null &&
        typeof error === 'object' &&
        'code' in error &&
        ['ECONNREFUSED', 'ENOTFOUND', 'ETIMEDOUT'].includes(error.code)
    );
}

if (typeof databaseUrl !== 'string' || databaseUrl === '' || !/^e2e_[0-9a-f]{32}$/u.test(namespace ?? '')) {
    await send({ type: 'failed', code: 'invalid_process_configuration' });
    process.exit(2);
}

await start(databaseUrl, namespace);
