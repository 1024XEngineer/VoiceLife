import { existsSync } from 'node:fs';
import { loadEnvFile } from 'node:process';
import { spawnSync } from 'node:child_process';

const build = spawnSync('pnpm', ['--silent', 'run', 'build'], {
    cwd: new URL('..', import.meta.url),
    encoding: 'utf8',
});
process.stderr.write(build.stdout ?? '');
process.stderr.write(build.stderr ?? '');
if (build.status !== 0) process.exit(5);

const environmentFile = new URL('../../../.env', import.meta.url);
if (existsSync(environmentFile)) loadEnvFile(environmentFile);

await import('./device-cli.mjs');
