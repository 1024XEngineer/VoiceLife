import type { ActionApplication } from '../application/api.js';
import type { GatewayLogEntry, GatewayLogger } from './http/gateway-http-server.js';

const DEFAULT_POLL_INTERVAL_MILLISECONDS = 1_000;

/** 后台收口过期提醒动作所需的依赖。 */
export interface ReminderActionExpiryWorkerOptions {
    /** 提醒动作应用服务；实现负责事务更新和 SSE 关闭。 */
    readonly actions: Pick<ActionApplication, 'expireDue' | 'recoverStaleProcessing'>;
    /** 接收脱敏生命周期与异常日志。 */
    readonly logger: GatewayLogger;
    /** 空闲扫描间隔；生产默认一秒。 */
    readonly pollIntervalMs?: number;
}

/** 独立扫描并收口过期 Action，避免设备断链时状态永久停在 processing。 */
export class ReminderActionExpiryWorker {
    private readonly pollIntervalMs: number;
    private readonly stopController = new AbortController();
    private running: Promise<void> | undefined;
    private wakeWaiter: (() => void) | undefined;

    /** @param options 应用服务、日志器和轮询配置。 */
    public constructor(private readonly options: ReminderActionExpiryWorkerOptions) {
        this.pollIntervalMs = positiveInteger(options.pollIntervalMs ?? DEFAULT_POLL_INTERVAL_MILLISECONDS);
    }

    /** 立即执行一次扫描并启动后续轮询；重复启动无副作用。 */
    public start(): void {
        if (this.running !== undefined || this.stopController.signal.aborted) return;
        this.running = this.runLoop();
    }

    /** 唤醒空闲 worker，供测试或未来事件驱动优化使用。 */
    public wake(): void {
        this.wakeWaiter?.();
    }

    /** 执行一次过期扫描，供确定性测试和轮询循环调用。
     * @returns 本次过期或恢复的动作数量。
     */
    public async runOnce(): Promise<number> {
        let expired = 0;
        try {
            expired = await this.options.actions.expireDue();
        } catch (error) {
            safeLog(this.options.logger, {
                level: 'error',
                event: 'reminder.action.expiry.poll.failed',
                errorCode: safeErrorCode(error),
            });
        }
        if (expired > 0) {
            safeLog(this.options.logger, {
                level: 'info',
                event: 'reminder.action.expiry.expired',
                count: expired,
            });
        }
        let recovered = 0;
        try {
            recovered = await this.options.actions.recoverStaleProcessing();
        } catch (error) {
            safeLog(this.options.logger, {
                level: 'error',
                event: 'reminder.action.recovery.poll.failed',
                errorCode: safeErrorCode(error),
            });
        }
        if (recovered > 0) {
            safeLog(this.options.logger, {
                level: 'info',
                event: 'reminder.action.processing.recovered',
                count: recovered,
            });
        }
        return expired + recovered;
    }

    /** 停止轮询并等待当前扫描完成。
     * @returns 当前扫描和轮询都停止后的 Promise。
     */
    public async close(): Promise<void> {
        this.stopController.abort();
        this.wake();
        await this.running;
    }

    private async runLoop(): Promise<void> {
        safeLog(this.options.logger, { level: 'info', event: 'reminder.action.expiry.started' });
        while (!this.stopController.signal.aborted) {
            try {
                await this.runOnce();
            } catch (error) {
                safeLog(this.options.logger, {
                    level: 'error',
                    event: 'reminder.action.expiry.poll.failed',
                    errorCode: safeErrorCode(error),
                });
            }
            if (!this.stopController.signal.aborted) await this.waitForPoll();
        }
        safeLog(this.options.logger, { level: 'info', event: 'reminder.action.expiry.stopped' });
    }

    private waitForPoll(): Promise<void> {
        return new Promise((resolve) => {
            let settled = false;
            const finish = (): void => {
                if (settled) return;
                settled = true;
                clearTimeout(timer);
                this.stopController.signal.removeEventListener('abort', finish);
                this.wakeWaiter = undefined;
                resolve();
            };
            const timer = setTimeout(finish, this.pollIntervalMs);
            timer.unref();
            this.wakeWaiter = finish;
            this.stopController.signal.addEventListener('abort', finish, { once: true });
        });
    }
}

function positiveInteger(value: number): number {
    if (!Number.isSafeInteger(value) || value <= 0) throw new Error('Expiry worker option must be a positive integer');
    return value;
}

function safeErrorCode(error: unknown): string {
    return error instanceof Error ? error.name : 'unknown_error';
}

function safeLog(logger: GatewayLogger, entry: GatewayLogEntry): void {
    try {
        logger.log(entry);
    } catch {
        // Observability failures must never stop action expiry recovery.
    }
}
