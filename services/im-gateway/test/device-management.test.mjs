import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import process from 'node:process';
import { test } from 'node:test';

import { createMockImGateway } from '../dist/app/create-im-gateway.js';
import { resolveDatabaseConnectionUrl } from '../dist/app/gateway-process.js';
import { DeviceManagementError, DeviceManagementService, tokenDigest } from '../dist/application/device-management.js';
import { DatabaseDeviceAuthenticationPort } from '../dist/infrastructure/security/production-ports.js';
import { FixedClock } from '../dist/infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../dist/infrastructure/persistence/in-memory.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';

const TOKENS = [
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',
    'BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB',
    'CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC',
];

function isPostgresUnavailable(error) {
    return (
        error !== null &&
        typeof error === 'object' &&
        'code' in error &&
        ['ECONNREFUSED', 'ENOTFOUND', 'ETIMEDOUT'].includes(error.code)
    );
}

function service(tokens = [...TOKENS]) {
    const unitOfWork = new InMemoryImUnitOfWork();
    const clock = new FixedClock();
    return {
        unitOfWork,
        clock,
        devices: new DeviceManagementService(unitOfWork, clock, () => tokens.shift()),
    };
}

test('device create stores only a 32-byte digest and list never returns credentials', async () => {
    const { unitOfWork, devices } = service();
    const created = await devices.create('user-一', 'device-1');
    assert.equal(created.deviceToken, TOKENS[0]);
    const stored = await unitOfWork.transaction((tx) => tx.devices.findById('device-1'));
    assert.equal(stored.tokenDigest.byteLength, 32);
    assert.deepEqual([...stored.tokenDigest], [...tokenDigest(TOKENS[0])]);
    assert.equal(JSON.stringify(stored).includes(TOKENS[0]), false);
    const listed = await devices.list('user-一');
    assert.equal('deviceToken' in listed[0], false);
    assert.equal('tokenDigest' in listed[0], false);
});

test('token digest collisions retry and exhaustion returns a sanitized internal failure', async () => {
    const unitOfWork = new InMemoryImUnitOfWork();
    const devices = new DeviceManagementService(unitOfWork, new FixedClock(), () => TOKENS[0]);
    await devices.create('user-1', 'device-1');
    await assert.rejects(
        () => devices.create('user-1', 'device-2'),
        (error) =>
            error instanceof DeviceManagementError &&
            error.kind === 'internal' &&
            error.message === 'Could not issue a unique device token',
    );
});

test('database authentication requires exactly 43 base64url token characters and active status', async () => {
    const { unitOfWork, devices } = service();
    await devices.create('user-1', 'device-1');
    const authentication = new DatabaseDeviceAuthenticationPort(unitOfWork);
    assert.deepEqual(await authentication.authenticate(`Bearer ${TOKENS[0]}`), {
        deviceId: 'device-1',
        userId: 'user-1',
    });
    for (const authorization of [
        '',
        `bearer ${TOKENS[0]}`,
        `Bearer ${TOKENS[0]}=`,
        `Bearer ${TOKENS[0]}x`,
        `Bearer ${TOKENS[1]}`,
    ]) {
        await assert.rejects(
            () => authentication.authenticate(authorization),
            (error) => error.code === 'unauthorized',
        );
    }
    await devices.revoke('device-1');
    await assert.rejects(
        () => authentication.authenticate(`Bearer ${TOKENS[0]}`),
        (error) => error.code === 'unauthorized',
    );
});

test('rotate invalidates the old token while preserving owner, and revoked devices cannot rotate', async () => {
    const { unitOfWork, devices } = service();
    await devices.create('user-1', 'device-1');
    const rotated = await devices.rotateToken('device-1');
    assert.equal(rotated.deviceToken, TOKENS[1]);
    const authentication = new DatabaseDeviceAuthenticationPort(unitOfWork);
    await assert.rejects(
        () => authentication.authenticate(`Bearer ${TOKENS[0]}`),
        (error) => error.code === 'unauthorized',
    );
    assert.equal((await authentication.authenticate(`Bearer ${TOKENS[1]}`)).userId, 'user-1');
    await devices.revoke('device-1');
    await assert.rejects(
        () => devices.rotateToken('device-1'),
        (error) => error instanceof DeviceManagementError && error.kind === 'conflict',
    );
});

test('revoke is idempotent and atomically cancels all pending sessions without removing history', async () => {
    const { unitOfWork, devices } = service();
    await devices.create('user-1', 'device-1');
    await unitOfWork.transaction(async (tx) => {
        await tx.pairingSessions.createPendingIfAbsent({
            id: 'pairing-1',
            displayCodeHash: 'hash-1',
            userId: 'user-1',
            deviceId: 'device-1',
            status: 'pending',
            expiresAt: '2026-08-03T00:10:00.000Z',
            createdAt: '2026-08-03T00:00:00.000Z',
        });
    });
    assert.equal((await devices.revoke('device-1')).status, 'revoked');
    assert.equal((await devices.revoke('device-1')).status, 'revoked');
    const session = await unitOfWork.transaction((tx) => tx.pairingSessions.findById('pairing-1'));
    assert.equal(session.status, 'cancelled');
});

test('device identifiers and owners enforce their byte/control-character contracts', async () => {
    const { devices } = service();
    for (const [userId, deviceId] of [
        ['', 'device'],
        ['user\u0085owner', 'device'],
        ['user\u009fowner', 'device'],
        ['user', 'has space'],
        ['user', '设备'],
        ['x'.repeat(129), 'device'],
    ]) {
        await assert.rejects(() => devices.create(userId, deviceId), TypeError);
    }
});

test('mock gateways sharing a UoW seed distinct valid device token digests', async () => {
    const unitOfWork = new InMemoryImUnitOfWork();
    const clock = new FixedClock();
    const first = createMockImGateway('device-a', clock, { unitOfWork });
    const second = createMockImGateway('device-b', clock, { unitOfWork });
    await first.application.channels.find('channel-missing');
    await second.application.channels.find('channel-missing');
    const devices = await unitOfWork.transaction((tx) => tx.devices.list());
    assert.equal(devices.length, 2);
    assert.equal(
        devices.every((device) => device.tokenDigest.byteLength === 32),
        true,
    );
    assert.notDeepEqual([...devices[0].tokenDigest], [...devices[1].tokenDigest]);
});

test('device CLI shares the validated production database host resolver', async () => {
    assert.equal(
        new URL(
            resolveDatabaseConnectionUrl({
                DATABASE_URL: 'postgresql://user:secret@127.0.0.1:5432/database',
                DATABASE_HOST: 'postgres',
            }),
        ).hostname,
        'postgres',
    );
    assert.throws(
        () =>
            resolveDatabaseConnectionUrl({
                DATABASE_URL: 'postgresql://user:secret@127.0.0.1:5432/database',
                DATABASE_HOST: 'postgres/unsafe',
            }),
        /DATABASE_HOST/u,
    );
    const source = await readFile(new URL('../scripts/device-cli.mjs', import.meta.url), 'utf8');
    assert.match(source, /resolveDatabaseConnectionUrl\(process\.env\)/u);
});

test('device CLI rejects invalid arguments before database configuration or migration', () => {
    const cases = [
        ['create'],
        ['create', '--user-id', 'user-1', '--device-id', 'has space'],
        ['list', '--user-id', 'user\u0085owner'],
        ['rotate-token', '--device-id', ''],
        ['revoke'],
    ];
    for (const args of cases) {
        const result = spawnSync(process.execPath, ['scripts/device-cli.mjs', ...args], {
            cwd: new URL('..', import.meta.url),
            encoding: 'utf8',
            env: { ...process.env, DATABASE_URL: '', DATABASE_HOST: '' },
        });
        assert.equal(result.status, 2, `${args.join(' ')} stderr: ${result.stderr}`);
        assert.equal(result.stdout, '');
        assert.match(result.stderr, /^(?:usage:|device-id|user-id)/u);
        assert.doesNotMatch(result.stderr, /DATABASE|ECONN|postgres/iu);
    }
});

test('production image preserves silent pnpm output for credential-safe device CLI execution', async () => {
    const dockerfile = await readFile(new URL('../Dockerfile', import.meta.url), 'utf8');
    const npmConfig = await readFile(new URL('../.npmrc', import.meta.url), 'utf8');
    assert.match(npmConfig, /^reporter=silent$/mu);
    assert.match(dockerfile, /COPY package\.json \.npmrc pnpm-lock\.yaml/u);
});

test('pnpm device keeps lifecycle output off stdout on a create argument failure', () => {
    const result = spawnSync('pnpm', ['device', '--', 'create'], {
        cwd: new URL('..', import.meta.url),
        encoding: 'utf8',
        env: { ...process.env, DATABASE_URL: '', DATABASE_HOST: '', FORCE_COLOR: '0' },
    });
    assert.equal(result.status, 2, result.stderr);
    assert.equal(result.stdout, '');
    assert.match(result.stderr, /^usage:/u);
    assert.doesNotMatch(result.stderr, /ELIFECYCLE|tsc -p|Device command failed/u);
});

test('pnpm device successful create emits only the one-time credential JSON on stdout', async () => {
    const fakeBin = await mkdtemp(join(tmpdir(), 'device-cli-bin-'));
    const credential = {
        deviceId: 'device-process-test',
        userId: 'user-process-test',
        deviceToken: TOKENS[0],
        status: 'active',
    };
    try {
        await writeFile(join(fakeBin, 'pnpm'), '#!/bin/sh\nexit 0\n', { mode: 0o755 });
        await writeFile(join(fakeBin, 'node'), `#!/bin/sh\nprintf '%s\\n' '${JSON.stringify(credential)}'\n`, {
            mode: 0o755,
        });
        const resolvedPnpm = spawnSync('/bin/sh', ['-c', 'command -v pnpm'], { encoding: 'utf8' }).stdout.trim();
        assert.notEqual(resolvedPnpm, '');
        const pnpmLauncher = await readFile(resolvedPnpm, 'utf8');
        const pnpmEntrypoint = /cmd-shim-target=(.+)$/mu.exec(pnpmLauncher)?.[1] ?? resolvedPnpm;
        const result = spawnSync(
            process.execPath,
            [pnpmEntrypoint, 'device', '--', 'create', '--user-id', credential.userId],
            {
                cwd: new URL('..', import.meta.url),
                encoding: 'utf8',
                env: { ...process.env, PATH: `${fakeBin}:${process.env.PATH ?? ''}`, FORCE_COLOR: '0' },
            },
        );
        assert.equal(result.status, 0, result.stderr);
        assert.equal(result.stdout, `${JSON.stringify(credential)}\n`);
        assert.equal(result.stderr, '');
    } finally {
        await rm(fakeBin, { recursive: true, force: true });
    }
});

test('device CLI performs create, list, rotate and revoke against PostgreSQL', async (context) => {
    const databaseUrl = process.env.DATABASE_URL ?? 'postgres://voicelife:voicelife@127.0.0.1:5432/voicelife';
    const probe = new PostgresImUnitOfWork(databaseUrl);
    try {
        await probe.migrate();
    } catch (error) {
        await probe.close().catch(() => undefined);
        if (isPostgresUnavailable(error)) {
            context.skip(`PostgreSQL unavailable: ${error instanceof Error ? error.name : 'unknown'}`);
            return;
        }
        throw error;
    }
    await probe.close();

    const suffix = `${Date.now()}-${Math.floor(Math.random() * 1_000_000)}`;
    const userId = `user-cli-${suffix}`;
    const deviceId = `device-cli-${suffix}`;
    const runCli = (args) =>
        spawnSync(process.execPath, ['scripts/device-cli.mjs', ...args], {
            cwd: new URL('..', import.meta.url),
            encoding: 'utf8',
            env: { ...process.env, DATABASE_URL: databaseUrl, DATABASE_HOST: '' },
        });

    const created = runCli(['create', '--user-id', userId, '--device-id', deviceId]);
    assert.equal(created.status, 0, created.stderr);
    assert.equal(created.stderr, '');
    const createResult = JSON.parse(created.stdout);
    assert.deepEqual(
        { deviceId: createResult.deviceId, userId: createResult.userId, status: createResult.status },
        { deviceId, userId, status: 'active' },
    );
    assert.match(createResult.deviceToken, /^[A-Za-z0-9_-]{43}$/u);

    const listed = runCli(['list', '--user-id', userId]);
    assert.equal(listed.status, 0, listed.stderr);
    assert.deepEqual(
        JSON.parse(listed.stdout).map((device) => device.deviceId),
        [deviceId],
    );
    assert.equal('deviceToken' in JSON.parse(listed.stdout)[0], false);

    const rotated = runCli(['rotate-token', '--device-id', deviceId]);
    assert.equal(rotated.status, 0, rotated.stderr);
    const rotateResult = JSON.parse(rotated.stdout);
    assert.match(rotateResult.deviceToken, /^[A-Za-z0-9_-]{43}$/u);
    assert.notEqual(rotateResult.deviceToken, createResult.deviceToken);

    const revoked = runCli(['revoke', '--device-id', deviceId]);
    assert.equal(revoked.status, 0, revoked.stderr);
    assert.deepEqual(
        { deviceId: JSON.parse(revoked.stdout).deviceId, status: JSON.parse(revoked.stdout).status },
        { deviceId, status: 'revoked' },
    );

    const rotateRevoked = runCli(['rotate-token', '--device-id', deviceId]);
    assert.equal(rotateRevoked.status, 4);
    assert.equal(rotateRevoked.stdout, '');
    assert.match(rotateRevoked.stderr, /Revoked device cannot rotate token/u);
});

test('device CLI wrapper returns its stable build-failure exit code without writing stdout', async () => {
    const fakeBin = await mkdtemp(join(tmpdir(), 'device-cli-build-failure-bin-'));
    try {
        await writeFile(join(fakeBin, 'pnpm'), '#!/bin/sh\nprintf build-stdout\nprintf build-stderr >&2\nexit 42\n', {
            mode: 0o755,
        });
        const result = spawnSync(process.execPath, ['scripts/run-device-cli.mjs'], {
            cwd: new URL('..', import.meta.url),
            encoding: 'utf8',
            env: { ...process.env, PATH: `${fakeBin}:${process.env.PATH ?? ''}` },
        });
        assert.equal(result.status, 5);
        assert.equal(result.stdout, '');
        assert.match(result.stderr, /build-stdout/u);
        assert.match(result.stderr, /build-stderr/u);
    } finally {
        await rm(fakeBin, { recursive: true, force: true });
    }
});
