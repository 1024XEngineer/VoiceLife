import type { GatewayLogEntry, GatewayLogger } from '../http/gateway-http-server.js';

/** 向标准输出逐行写入 JSON 的生产结构化日志器。 */
export class JsonLineGatewayLogger implements GatewayLogger {
    /** {@inheritDoc GatewayLogger.log} */
    public log(entry: GatewayLogEntry): void {
        process.stdout.write(`${JSON.stringify({ timestamp: new Date().toISOString(), ...entry })}\n`);
    }
}
