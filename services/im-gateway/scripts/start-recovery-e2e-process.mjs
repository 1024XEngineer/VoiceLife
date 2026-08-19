import { createKoishiGatewayRuntime } from '../dist/app/create-koishi-gateway.js';
import { DeliveryOutboxWorker } from '../dist/infrastructure/delivery-outbox-worker.js';
import { startGatewayHttpServer } from '../dist/infrastructure/http/gateway-http-server.js';
import {
    FixedClock,
    MockChannelCapabilities,
    MockDeliveryRenderer,
    MockPairingCodePort,
} from '../dist/infrastructure/mock-support.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';
import { DirectConversationResolver } from '../dist/infrastructure/production-support.js';
import { AesGcmActionTokenPort } from '../dist/infrastructure/security/aes-gcm-action-token.js';
import { DatabaseDeviceAuthenticationPort, UuidIdGenerator } from '../dist/infrastructure/security/production-ports.js';
import { SseActionCommandHub } from '../dist/infrastructure/sse/sse-action-command-hub.js';

import {
    RecoveryPlatformAdapter,
    actionTokenSecret,
    claimRecoveryEvents,
    createRecoveryTables,
    deviceFixture,
    publicDevice,
    recoverySnapshot,
    seedRecoveryFixtures,
} from './recovery-e2e-support.mjs';

const databaseUrl = process.env.E2E_DATABASE_URL;
const namespace = process.env.E2E_PREFIX;
const role = process.env.E2E_ROLE;
const initialNow = process.env.E2E_NOW;
const initialMode = process.env.E2E_PLATFORM_MODE ?? 'success';

async function start(connectionString, prefix, processRole, now, platformMode) {
    const target = deviceFixture(prefix, 'device');
    const isolation = deviceFixture(prefix, 'isolation');
    const channelId = `${prefix}_channel`;
    const clock = new FixedClock(now);
    const logs = [];
    const unitOfWork = new PostgresImUnitOfWork(connectionString);
    const replayBarrier = new ReplayBarrier();
    const overflowScopes = [];
    const actionStream = new SseActionCommandHub({
        maxQueueSize: 1,
        subscriptionOverflowed: (scope) => overflowScopes.push(scope),
    });
    const platform = new RecoveryPlatformAdapter(unitOfWork, clock, platformMode, async (point, deliveryId) => {
        await send({ type: 'injection', point, deliveryId });
    });
    let gateway;
    let server;
    let worker;
    let closing;

    const close = () => {
        if (closing !== undefined) return closing;
        closing = runCleanup([
            () => server?.close(),
            () => worker?.close(),
            () => gateway?.close(),
            () => unitOfWork.close(),
        ]);
        return closing;
    };

    const terminate = async (requestedExitCode) => {
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
        await createRecoveryTables(unitOfWork);
        if (processRole === 'gateway') await seedRecoveryFixtures(unitOfWork, prefix, clock.now(), target, isolation);
        gateway = createKoishiGatewayRuntime({
            dependencies: {
                unitOfWork,
                actionStream,
                actionTokens: new AesGcmActionTokenPort(actionTokenSecret(prefix)),
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
        wrapReplayWithBarrier(gateway.runtime.application.actions, replayBarrier);
        if (processRole === 'gateway') {
            await gateway.start();
            server = await startGatewayHttpServer({
                host: '127.0.0.1',
                port: 0,
                runtime: gateway.runtime,
                logger: { log: (entry) => logs.push(entry) },
                sseHeartbeatIntervalMs: 25,
                healthCheck: async () => {
                    await unitOfWork.runRaw('SELECT 1');
                    return { status: 'ok' };
                },
            });
        } else {
            worker = new DeliveryOutboxWorker({
                unitOfWork,
                dispatch: gateway.runtime.application.deliveryDispatch,
                clock,
                logger: { log: (entry) => logs.push(entry) },
                pollIntervalMs: 25,
            });
        }
        process.on('message', (message) => {
            void handleMessage(message, {
                unitOfWork,
                namespace: prefix,
                role: processRole,
                clock,
                platform,
                worker,
                gateway,
                replayBarrier,
                overflowScopes,
                logs,
                close,
            });
        });
        await send({
            type: 'ready',
            role: processRole,
            ...(server === undefined ? {} : { origin: server.origin }),
            target: publicDevice(target),
            isolation: publicDevice(isolation),
        });
    } catch {
        await send({ type: 'failed', code: 'recovery_process_start_failed' });
        await terminate(1);
    }
}

async function handleMessage(message, state) {
    if (typeof message !== 'object' || message === null || typeof message.requestId !== 'number') return;
    try {
        if (message.type === 'snapshot') {
            const snapshot = await recoverySnapshot(state.unitOfWork, state.namespace, message.eventId);
            await respond(message.requestId, { ...snapshot, logCount: state.logs.length });
            return;
        }
        if (message.type === 'run_once' && state.role === 'worker') {
            await respond(message.requestId, { claimed: await state.worker.runOnce() });
            return;
        }
        if (message.type === 'claim_only' && state.role === 'worker') {
            const events = await claimRecoveryEvents(state.unitOfWork, state.clock);
            await respond(message.requestId, { claimed: events.length });
            return;
        }
        if (message.type === 'advance' && state.role === 'worker') {
            state.clock.advanceMinutes(message.minutes);
            await respond(message.requestId, { now: state.clock.now() });
            return;
        }
        if (message.type === 'set_mode' && state.role === 'worker') {
            state.platform.setMode(message.mode);
            await respond(message.requestId, { mode: message.mode });
            return;
        }
        if (message.type === 'arm_replay' && state.role === 'gateway') {
            state.replayBarrier.arm();
            await respond(message.requestId, { armed: true });
            return;
        }
        if (message.type === 'release_replay' && state.role === 'gateway') {
            state.replayBarrier.release();
            await respond(message.requestId, { released: true });
            return;
        }
        if (message.type === 'backpressure' && state.role === 'gateway') {
            await respond(
                message.requestId,
                await runBackpressureProbe(state.gateway.actionStream, state.overflowScopes),
            );
            return;
        }
        if (message.type === 'stop') {
            await state.close();
            await respond(message.requestId, { stopped: true });
            process.disconnect();
            return;
        }
        await respond(message.requestId, undefined, 'unknown_process_command');
    } catch {
        await respond(message.requestId, undefined, 'process_command_failed');
    }
}

function wrapReplayWithBarrier(actions, barrier) {
    const replayPending = actions.replayPending.bind(actions);
    actions.replayPending = async (...args) => {
        await barrier.wait();
        return replayPending(...args);
    };
}

class ReplayBarrier {
    arm() {
        this.armed = true;
        this.promise = new Promise((resolve) => {
            this.resolve = resolve;
        });
    }

    release() {
        this.resolve?.();
        this.armed = false;
        this.resolve = undefined;
    }

    async wait() {
        if (!this.armed) return;
        await send({ type: 'injection', point: 'replay_after_subscribe' });
        await this.promise;
    }
}

async function runBackpressureProbe(stream, overflowScopes) {
    const scope = {
        deviceId: 'backpressure-device',
        reminderTriggerId: 'backpressure-trigger',
        expiresAt: '2099-08-18T00:10:00.000Z',
    };
    const iterator = stream.subscribe(scope)[Symbol.asyncIterator]();
    const base = {
        schemaVersion: '1',
        commandId: 'backpressure-action-1',
        operationId: 'backpressure-operation-1',
        correlationId: 'backpressure-correlation',
        ...scope,
        action: 'acknowledge',
        occurredAt: '2026-08-18T00:00:00.000Z',
    };
    await stream.publish(base);
    await stream.publish({ ...base, commandId: 'backpressure-action-2', operationId: 'backpressure-operation-2' });
    const result = await iterator.next();
    return { closed: result.done === true, overflowCount: overflowScopes.length };
}

async function respond(requestId, data, error) {
    await send({ type: 'response', requestId, ...(error === undefined ? { data } : { error }) });
}

function send(message) {
    return new Promise((resolve) => {
        if (!process.connected) return resolve();
        process.send(message, () => resolve());
    });
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
    if (errors.length > 0) throw new AggregateError(errors, 'recovery_process_cleanup_failed');
}

if (
    typeof databaseUrl !== 'string' ||
    databaseUrl === '' ||
    typeof namespace !== 'string' ||
    !/^e2e_[0-9a-f]{32}_[0-9]+$/u.test(namespace) ||
    !['gateway', 'worker'].includes(role) ||
    typeof initialNow !== 'string' ||
    !Number.isFinite(Date.parse(initialNow))
) {
    await send({ type: 'failed', code: 'invalid_process_configuration' });
    process.exit(2);
}

await start(databaseUrl, namespace, role, initialNow, initialMode);
