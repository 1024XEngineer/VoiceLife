import process from 'node:process';

import { GatewayConfigurationError, startConfiguredGatewayProcess } from '../dist/app/gateway-process.js';

try {
    const gateway = await startConfiguredGatewayProcess(process.env);
    let stopping;
    const stop = () => {
        stopping ??= gateway.close();
        void stopping.catch(() => {
            process.exitCode = 1;
        });
    };
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
} catch (error) {
    const failure = {
        level: 'error',
        event: 'gateway.start.failed',
        errorCode: error instanceof GatewayConfigurationError ? 'invalid_configuration' : 'startup_failed',
        ...(error instanceof GatewayConfigurationError ? { message: error.message } : {}),
    };
    process.stderr.write(`${JSON.stringify(failure)}\n`);
    process.exitCode = 1;
}
