import { randomUUID } from 'node:crypto';
import { fork } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Client } from 'pg';

import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';

import { createRecoveryTables, recoverySnapshot } from './recovery-e2e-support.mjs';
import {
    evidenceState,
    FULL_SCENARIOS,
    QUICK_SCENARIOS,
    RecoveryAssertionError,
    SCENARIOS,
} from './recovery-e2e-scenarios.mjs';

const PROCESS_SCRIPT = fileURLToPath(new URL('./start-recovery-e2e-process.mjs', import.meta.url));
const IPC_TIMEOUT_MS = 5000;

export async function runRecoveryE2e({
    databaseUrl = process.env.DATABASE_URL,
    runId = process.env.E2E_RUN_ID ?? randomUUID().replaceAll('-', ''),
    scenarioNames = selectedScenarios(process.argv.slice(2), process.env),
    evidencePath = process.env.E2E_RECOVERY_EVIDENCE,
} = {}) {
    if (typeof databaseUrl !== 'string' || databaseUrl.trim() === '') throw new Error('database_required');
    if (!/^[0-9a-f]{32}$/u.test(runId)) throw new Error('run_id_invalid');
    validateScenarios(scenarioNames);
    const admin = new Client({ connectionString: databaseUrl });
    const details = [];
    let primaryError;
    let cleanupFailed = false;
    await admin.connect();
    try {
        for (const [index, name] of scenarioNames.entries()) {
            const prefix = `e2e_${runId}_${index + 1}`;
            const scopedDatabaseUrl = new URL(databaseUrl);
            scopedDatabaseUrl.searchParams.set('options', `-c search_path=${prefix}`);
            await admin.query(`CREATE SCHEMA "${prefix}"`);
            const context = new ScenarioContext(prefix, scopedDatabaseUrl.toString());
            let scenarioError;
            try {
                await context.initialize();
                const outcome = await SCENARIOS[name](context);
                details.push({ name, status: 'passed', ...outcome });
            } catch (error) {
                scenarioError = error;
                const snapshot = await context.lastSnapshot().catch(() => emptySnapshot());
                details.push({
                    name,
                    status: 'failed',
                    messageCode: safeErrorCode(error),
                    injectionPoint: 'last_persisted_state',
                    assertions: 0,
                    state: evidenceState(snapshot),
                });
            } finally {
                try {
                    await context.close();
                } catch {
                    cleanupFailed = true;
                }
                try {
                    await admin.query(`DROP SCHEMA "${prefix}" CASCADE`);
                } catch {
                    cleanupFailed = true;
                }
            }
            if (scenarioError !== undefined) {
                primaryError = scenarioError;
                break;
            }
        }
    } finally {
        await admin.end().catch(() => {
            cleanupFailed = true;
        });
    }
    const summary = summarize(scenarioNames, details, primaryError, cleanupFailed);
    if (evidencePath !== undefined) await writeDetailedEvidence(evidencePath, runId, summary, details);
    if (cleanupFailed) throw new RecoveryRunError('cleanup_failed', summary);
    if (primaryError !== undefined) throw new RecoveryRunError(safeErrorCode(primaryError), summary);
    return summary;
}

class ScenarioContext {
    constructor(prefix, databaseUrl) {
        this.prefix = prefix;
        this.databaseUrl = databaseUrl;
        this.children = new Set();
        this.unitOfWork = new PostgresImUnitOfWork(databaseUrl);
    }

    async initialize() {
        await this.unitOfWork.migrate();
        await createRecoveryTables(this.unitOfWork);
    }

    startGateway(now) {
        return this.start('gateway', 'success', now);
    }

    startWorker(mode, now) {
        return this.start('worker', mode, now);
    }

    async start(role, mode, now) {
        const child = fork(PROCESS_SCRIPT, [], {
            env: {
                E2E_DATABASE_URL: this.databaseUrl,
                E2E_PREFIX: this.prefix,
                E2E_ROLE: role,
                E2E_NOW: now,
                E2E_PLATFORM_MODE: mode,
                NODE_ENV: 'test',
            },
            stdio: ['ignore', 'pipe', 'pipe', 'ipc'],
        });
        child.stdout?.resume();
        child.stderr?.resume();
        this.children.add(child);
        const ready = await waitForMessage(child, (message) => message.type === 'ready' || message.type === 'failed');
        if (ready.type === 'failed') throw new Error(ready.code);
        return { child, ready };
    }

    request(child, type, input = {}) {
        const requestId = ScenarioContext.nextRequestId++;
        const response = waitForMessage(
            child,
            (message) => message.type === 'response' && message.requestId === requestId,
        );
        child.send({ type, requestId, ...input });
        return response.then((message) => {
            if (typeof message.error === 'string') throw new Error(message.error);
            return message.data;
        });
    }

    waitForInjection(child, point) {
        return waitForMessage(child, (message) => message.type === 'injection' && message.point === point);
    }

    snapshot(child, eventId) {
        return this.request(child, 'snapshot', { eventId });
    }

    lastSnapshot() {
        return recoverySnapshot(this.unitOfWork, this.prefix);
    }

    async stop(child) {
        if (child.exitCode !== null || child.signalCode !== null) return;
        await this.request(child, 'stop');
        await waitForExit(child);
        this.children.delete(child);
        if (child.exitCode !== 0) throw new Error('process_cleanup_failed');
    }

    async kill(child) {
        if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
        await waitForExit(child);
        this.children.delete(child);
    }

    async close() {
        const errors = [];
        for (const child of [...this.children]) {
            try {
                await this.stop(child);
            } catch (error) {
                errors.push(error);
                await this.kill(child).catch((killError) => errors.push(killError));
            }
        }
        await this.unitOfWork.close().catch((error) => errors.push(error));
        if (errors.length > 0) throw new AggregateError(errors, 'scenario_cleanup_failed');
    }
}
ScenarioContext.nextRequestId = 1;

function waitForMessage(child, predicate, timeoutMs = IPC_TIMEOUT_MS) {
    return new Promise((resolve, reject) => {
        const finish = (error, value) => {
            globalThis.clearTimeout(timer);
            child.off('message', onMessage);
            child.off('exit', onExit);
            if (error === undefined) resolve(value);
            else reject(error);
        };
        const onMessage = (message) => {
            if (typeof message === 'object' && message !== null && predicate(message)) finish(undefined, message);
        };
        const onExit = () => finish(new Error('recovery_process_exited'));
        const timer = globalThis.setTimeout(() => finish(new Error('recovery_process_timeout')), timeoutMs);
        child.on('message', onMessage);
        child.once('exit', onExit);
    });
}

function waitForExit(child, timeoutMs = IPC_TIMEOUT_MS) {
    if (child.exitCode !== null || child.signalCode !== null) return Promise.resolve();
    return new Promise((resolve, reject) => {
        const timer = globalThis.setTimeout(() => reject(new Error('recovery_process_exit_timeout')), timeoutMs);
        child.once('exit', () => {
            globalThis.clearTimeout(timer);
            resolve();
        });
    });
}

function selectedScenarios(argv, environment) {
    let suite = environment.E2E_RECOVERY_SUITE ?? 'quick';
    const explicit = [];
    for (let index = 0; index < argv.length; index += 1) {
        if (argv[index] === '--suite') suite = argv[++index];
        else if (argv[index] === '--scenario') explicit.push(argv[++index]);
        else throw new Error('invalid_arguments');
    }
    if (typeof environment.E2E_RECOVERY_SCENARIO === 'string') {
        explicit.push(...environment.E2E_RECOVERY_SCENARIO.split(',').filter(Boolean));
    }
    if (explicit.length > 0) return explicit;
    if (suite === 'quick') return QUICK_SCENARIOS;
    if (suite === 'full') return FULL_SCENARIOS;
    throw new Error('invalid_suite');
}

function validateScenarios(names) {
    if (!Array.isArray(names) || names.length === 0 || names.some((name) => !(name in SCENARIOS))) {
        throw new Error('invalid_scenario');
    }
}

function summarize(requested, details, primaryError, cleanupFailed) {
    return {
        status: primaryError === undefined && !cleanupFailed ? 'passed' : 'failed',
        scenarioCount: details.filter((item) => item.status === 'passed').length,
        requestedCount: requested.length,
        assertions: details.reduce((total, item) => total + item.assertions, 0),
        deliveryCount: details.reduce((total, item) => total + item.state.deliveries.length, 0),
        attemptCount: details.reduce((total, item) => total + item.state.attempts.length, 0),
        platformSendCount: details.reduce((total, item) => total + item.state.platformSendCount, 0),
        scenarios: details.map((item) => item.name),
    };
}

async function writeDetailedEvidence(path, runId, summary, scenarios) {
    const document = { schemaVersion: 1, runId, ...summary, scenarios };
    await mkdir(dirname(path), { recursive: true });
    await writeFile(path, `${JSON.stringify(document, null, 2)}\n`, { encoding: 'utf8', mode: 0o600 });
}

function emptySnapshot() {
    return { deliveries: [], attempts: [], outbox: [], actions: [], platformCalls: [], platformSends: [] };
}

function safeErrorCode(error) {
    if (error instanceof RecoveryAssertionError) return error.code;
    return error instanceof Error && /^[a-z0-9_]+$/u.test(error.message) ? error.message : 'recovery_e2e_failed';
}

function infrastructureFailure(error) {
    return (
        error !== null &&
        typeof error === 'object' &&
        'code' in error &&
        ['ECONNREFUSED', 'ENOTFOUND', 'ETIMEDOUT', '28P01', 'EACCES', 'ENOSPC'].includes(error.code)
    );
}

export class RecoveryRunError extends Error {
    constructor(code, summary) {
        super(code);
        this.code = code;
        this.summary = summary;
    }
}

if (import.meta.url === `file://${process.argv[1]}`) {
    try {
        console.log(JSON.stringify(await runRecoveryE2e()));
    } catch (error) {
        console.log(
            JSON.stringify(
                error instanceof RecoveryRunError
                    ? error.summary
                    : { status: 'failed', messageCode: safeErrorCode(error) },
            ),
        );
        console.error(
            error instanceof RecoveryRunError && error.code === 'cleanup_failed'
                ? 'host_recovery_cleanup_failed'
                : infrastructureFailure(error)
                  ? 'host_recovery_infrastructure_failed'
                  : 'host_recovery_e2e_failed',
        );
        process.exitCode = 1;
    }
}
