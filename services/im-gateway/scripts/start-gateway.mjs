import process from 'node:process';

import { startConfiguredGatewayProcess } from '../dist/app/gateway-process.js';

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
    const errorCode = error instanceof Error ? error.name : 'unknown_error';
    process.stderr.write(`${JSON.stringify({ level: 'error', event: 'gateway.start.failed', errorCode })}\n`);
    process.exitCode = 1;
}
