import process from 'node:process';

import { startConfiguredWechatDevHarness } from '../dist/app/wechat-dev-runtime.js';

try {
    const harness = await startConfiguredWechatDevHarness(process.env);
    console.log(`WeChat development harness listening at ${harness.origin}`);
    console.log('Public webhook path: /wechat');
    console.log('Public Action UI path: /voicelife/reminder-actions/:token');
    console.log('Test sends require the local DEVICE_TOKEN-protected endpoint.');

    let stopping = false;
    const stop = async () => {
        if (stopping) return;
        stopping = true;
        await harness.close();
    };
    process.once('SIGINT', () => void stop());
    process.once('SIGTERM', () => void stop());
} catch (error) {
    const message = error instanceof Error ? error.message : 'unknown startup failure';
    console.error(`Unable to start WeChat development harness: ${message}`);
    process.exitCode = 1;
}
