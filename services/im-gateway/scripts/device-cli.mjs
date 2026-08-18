#!/usr/bin/env node
import process from 'node:process';

import { resolveDatabaseConnectionUrl } from '../dist/app/gateway-process.js';
import {
    DeviceManagementError,
    DeviceManagementService,
    validateDeviceId,
    validateUserId,
} from '../dist/application/device-management.js';
import { PostgresImUnitOfWork } from '../dist/infrastructure/persistence/postgres.js';
import { SystemClock } from '../dist/infrastructure/production-support.js';

const EXIT = { argument: 2, notFound: 3, conflict: 4, internal: 5 };

function usage() {
    throw new TypeError('usage: device <create|list|rotate-token|revoke> [options]');
}

function options(args) {
    const result = new Map();
    for (let index = 0; index < args.length; index += 2) {
        const key = args[index];
        const value = args[index + 1];
        if (!key?.startsWith('--') || value === undefined || value.startsWith('--') || result.has(key)) usage();
        result.set(key, value);
    }
    return result;
}

function parseInvocation(argv) {
    if (argv[0] === '--') argv.shift();
    const [command, ...rest] = argv;
    if (command === undefined) usage();
    const parsed = options(rest);
    const allowed =
        command === 'create' ? ['--user-id', '--device-id'] : command === 'list' ? ['--user-id'] : ['--device-id'];
    if (
        !['create', 'list', 'rotate-token', 'revoke'].includes(command) ||
        [...parsed.keys()].some((key) => !allowed.includes(key))
    )
        usage();

    if (command === 'create') {
        const userId = parsed.get('--user-id');
        if (userId === undefined) usage();
        validateUserId(userId);
        const deviceId = parsed.get('--device-id');
        if (deviceId !== undefined) validateDeviceId(deviceId);
        return { command, userId, deviceId };
    }
    if (command === 'list') {
        const userId = parsed.get('--user-id');
        if (userId !== undefined) validateUserId(userId);
        return { command, userId };
    }
    const deviceId = parsed.get('--device-id');
    if (deviceId === undefined) usage();
    validateDeviceId(deviceId);
    return { command, deviceId };
}

async function run() {
    const invocation = parseInvocation(process.argv.slice(2));
    const uow = new PostgresImUnitOfWork(resolveDatabaseConnectionUrl(process.env));
    try {
        await uow.migrate();
        const devices = new DeviceManagementService(uow, new SystemClock());
        let result;
        if (invocation.command === 'create') {
            result = await devices.create(invocation.userId, invocation.deviceId);
        } else if (invocation.command === 'list') {
            result = await devices.list(invocation.userId);
        } else if (invocation.command === 'rotate-token') {
            result = await devices.rotateToken(invocation.deviceId);
        } else {
            result = await devices.revoke(invocation.deviceId);
        }
        process.stdout.write(`${JSON.stringify(result)}\n`);
    } finally {
        await uow.close();
    }
}

try {
    await run();
} catch (error) {
    if (error instanceof TypeError) {
        process.stderr.write(`${error.message}\n`);
        process.exitCode = EXIT.argument;
    } else if (error instanceof DeviceManagementError) {
        process.stderr.write(`${error.message}\n`);
        process.exitCode =
            error.kind === 'not_found' ? EXIT.notFound : error.kind === 'conflict' ? EXIT.conflict : EXIT.internal;
    } else {
        process.stderr.write('Device command failed\n');
        process.exitCode = EXIT.internal;
    }
}
