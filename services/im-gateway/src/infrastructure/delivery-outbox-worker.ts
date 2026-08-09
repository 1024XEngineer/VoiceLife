import type { DeliveryDispatchApplication } from '../application/api.js';
import { unsafeId, type DeliveryId } from '../contracts/ids.js';
import type { Delivery, ImOutboxEvent } from '../domain/models.js';
import type { GatewayLogEntry, GatewayLogger } from './http/gateway-http-server.js';
import type { Clock } from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';
import { ImGatewayError } from '../shared/errors.js';
import type { IsoDateTime } from '../shared/types.js';

const DELIVERY_EVENT_TYPES = [
    'im.delivery.requested',
    'im.delivery.retry-scheduled',
    'im.delivery.retry-requested',
] as const;
const DEFAULT_BATCH_SIZE = 20;
const DEFAULT_LEASE_MILLISECONDS = 2 * 60_000;
const DEFAULT_POLL_INTERVAL_MILLISECONDS = 1000;

/** 持久化 Delivery outbox worker 的装配参数。 */
export interface DeliveryOutboxWorkerOptions {
    /** 包含 Delivery 与 Outbox 仓储的事务工作单元。 */
    readonly unitOfWork: ImUnitOfWork;
    /** 执行带领取隔离的 Delivery 派发应用服务。 */
    readonly dispatch: Pick<DeliveryDispatchApplication, 'dispatch'>;
    /** 提供可测试的 UTC 时间。 */
    readonly clock: Clock;
    /** 接收脱敏 worker 生命周期与投递日志。 */
    readonly logger: GatewayLogger;
    /** 空闲轮询间隔；生产默认一秒。 */
    readonly pollIntervalMs?: number;
    /** 单次领取事件数；生产默认二十。 */
    readonly batchSize?: number;
    /** Outbox 领取租约；生产默认两分钟。 */
    readonly leaseMs?: number;
}

/** 持续领取事务性 Outbox，恢复并派发 pending/retryable Delivery。 */
export class DeliveryOutboxWorker {
    private readonly pollIntervalMs: number;
    private readonly batchSize: number;
    private readonly leaseMs: number;
    private readonly stopController = new AbortController();
    private running: Promise<void> | undefined;
    private wakeWaiter: (() => void) | undefined;

    /** @param options 持久化、派发、时钟与观测依赖。 */
    public constructor(private readonly options: DeliveryOutboxWorkerOptions) {
        this.pollIntervalMs = positiveInteger(options.pollIntervalMs ?? DEFAULT_POLL_INTERVAL_MILLISECONDS);
        this.batchSize = positiveInteger(options.batchSize ?? DEFAULT_BATCH_SIZE);
        this.leaseMs = positiveInteger(options.leaseMs ?? DEFAULT_LEASE_MILLISECONDS);
    }

    /** 启动时立即恢复持久化事件，随后持续轮询；重复调用无副作用。 */
    public start(): void {
        if (this.running !== undefined || this.stopController.signal.aborted) return;
        this.running = this.runLoop();
    }

    /** 唤醒空闲 worker，使新提交的事务性事件无需等待下一轮定时器。 */
    public wake(): void {
        this.wakeWaiter?.();
    }

    /**
     * 领取并处理一批当前可用事件，供确定性测试和循环内部调用。
     * @returns 本批领取的事件数量。
     */
    public async runOnce(): Promise<number> {
        const now = this.options.clock.now();
        const leaseUntil = addMilliseconds(now, this.leaseMs);
        const events = await this.options.unitOfWork.transaction((context) =>
            context.outbox.claimPending(DELIVERY_EVENT_TYPES, now, leaseUntil, this.batchSize),
        );
        for (const event of events) await this.process(event);
        return events.length;
    }

    /** @returns 当前一批完成且轮询循环停止后兑现的 Promise。 */
    public async close(): Promise<void> {
        this.stopController.abort();
        this.wake();
        await this.running;
    }

    private async runLoop(): Promise<void> {
        safeLog(this.options.logger, { level: 'info', event: 'delivery.worker.started' });
        while (!this.stopController.signal.aborted) {
            try {
                await this.runOnce();
            } catch (error) {
                safeLog(this.options.logger, {
                    level: 'error',
                    event: 'delivery.worker.poll.failed',
                    errorCode: safeErrorCode(error),
                });
            }
            if (!this.stopController.signal.aborted) await this.waitForPoll();
        }
        safeLog(this.options.logger, { level: 'info', event: 'delivery.worker.stopped' });
    }

    private async process(event: ImOutboxEvent): Promise<void> {
        const deliveryId = unsafeId<DeliveryId>(event.aggregateId);
        const delivery = await this.options.unitOfWork.transaction((context) =>
            context.deliveries.findById(deliveryId),
        );
        if (delivery === undefined) {
            await this.fail(event, deliveryId, 'delivery_not_found');
            return;
        }
        if (isConsumed(delivery)) {
            await this.publish(event);
            return;
        }
        try {
            const result = await this.options.dispatch.dispatch(deliveryId);
            if (result.status === 'sending') {
                this.logDeferred(result, 'delivery_claim_active');
                return;
            }
            await this.publish(event);
            safeLog(this.options.logger, {
                level: 'info',
                event: 'delivery.worker.dispatched',
                deliveryId: result.id,
                correlationId: result.correlationId,
            });
        } catch (error) {
            if (error instanceof ImGatewayError && error.code !== 'invalid_transition') {
                await this.fail(event, deliveryId, error.code);
                return;
            }
            this.logDeferred(delivery, safeErrorCode(error));
        }
    }

    private publish(event: ImOutboxEvent): Promise<void> {
        return this.options.unitOfWork.transaction((context) =>
            context.outbox.markPublished(event.id, this.options.clock.now()),
        );
    }

    private async fail(event: ImOutboxEvent, deliveryId: DeliveryId, errorCode: string): Promise<void> {
        await this.options.unitOfWork.transaction((context) => context.outbox.markFailed(event.id));
        safeLog(this.options.logger, {
            level: 'error',
            event: 'delivery.worker.event.failed',
            deliveryId,
            errorCode,
        });
    }

    private logDeferred(delivery: Delivery, errorCode: string): void {
        safeLog(this.options.logger, {
            level: 'warn',
            event: 'delivery.worker.deferred',
            deliveryId: delivery.id,
            correlationId: delivery.correlationId,
            errorCode,
        });
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

function isConsumed(delivery: Delivery): boolean {
    return ['accepted', 'delivered', 'permanent_failed', 'dead_letter'].includes(delivery.status);
}

function addMilliseconds(value: IsoDateTime, milliseconds: number): IsoDateTime {
    return new Date(Date.parse(value) + milliseconds).toISOString() as IsoDateTime;
}

function positiveInteger(value: number): number {
    if (!Number.isSafeInteger(value) || value <= 0)
        throw new Error('Delivery worker option must be a positive integer');
    return value;
}

function safeErrorCode(error: unknown): string {
    return error instanceof ImGatewayError ? error.code : error instanceof Error ? error.name : 'unknown_error';
}

function safeLog(logger: GatewayLogger, entry: GatewayLogEntry): void {
    try {
        logger.log(entry);
    } catch {
        // Observability failures must never stop persistent delivery recovery.
    }
}
