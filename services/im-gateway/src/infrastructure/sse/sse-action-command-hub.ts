import type { ActionId } from '../../contracts/ids.js';
import type { ReminderActionCommand } from '../../contracts/device-gateway.js';
import type {
    ActionCommandStreamPort,
    ActionStreamCloseScope,
    ActionStreamSubscription,
} from '../../ports/external.js';
import { ImGatewayError } from '../../shared/errors.js';

const MAX_TIMER_DELAY_MS = 2_147_483_647;
const DEFAULT_MAX_SUBSCRIPTIONS = 1000;
const DEFAULT_MAX_SUBSCRIPTIONS_PER_DEVICE = 4;
const DEFAULT_MAX_SUBSCRIPTIONS_PER_SCOPE = 2;
const DEFAULT_MAX_QUEUE_SIZE = 8;

/** SSE Hub 的生产容量边界和溢出观测配置。 */
export interface SseActionCommandHubOptions {
    readonly maxSubscriptions?: number;
    readonly maxSubscriptionsPerDevice?: number;
    readonly maxSubscriptionsPerScope?: number;
    readonly maxQueueSize?: number;
    /**
     * 慢订阅者队列溢出时接收不含命令载荷的作用域通知。
     * @param scope 发生溢出的设备与提醒窗口。
     */
    readonly subscriptionOverflowed?: (scope: ActionStreamCloseScope) => void;
}

interface ActionScope {
    readonly expiresAt: number;
    readonly key: string;
}

/** 为设备 SSE 连接提供实时命令扇出、作用域隔离与生命周期关闭的 Hub。 */
export class SseActionCommandHub implements ActionCommandStreamPort {
    private readonly subscriptions = new Set<HubSubscription>();

    private readonly actionScopes = new Map<ActionId, ActionScope>();

    private readonly scopeActions = new Map<string, Set<ActionId>>();

    private readonly closedActions = new Map<ActionId, number>();

    private readonly maxSubscriptions: number;

    private readonly maxSubscriptionsPerDevice: number;

    private readonly maxSubscriptionsPerScope: number;

    private readonly maxQueueSize: number;

    /** @param options 总连接、单设备、单窗口与慢客户端队列的容量边界。 */
    public constructor(private readonly options: SseActionCommandHubOptions = {}) {
        this.maxSubscriptions = positiveInteger(options.maxSubscriptions ?? DEFAULT_MAX_SUBSCRIPTIONS);
        this.maxSubscriptionsPerDevice = positiveInteger(
            options.maxSubscriptionsPerDevice ?? DEFAULT_MAX_SUBSCRIPTIONS_PER_DEVICE,
        );
        this.maxSubscriptionsPerScope = positiveInteger(
            options.maxSubscriptionsPerScope ?? DEFAULT_MAX_SUBSCRIPTIONS_PER_SCOPE,
        );
        this.maxQueueSize = positiveInteger(options.maxQueueSize ?? DEFAULT_MAX_QUEUE_SIZE);
        if (this.maxSubscriptionsPerDevice > this.maxSubscriptions) {
            throw new Error('SSE per-device subscription limit cannot exceed the global limit');
        }
        if (this.maxSubscriptionsPerScope > this.maxSubscriptionsPerDevice) {
            throw new Error('SSE per-scope subscription limit cannot exceed the per-device limit');
        }
    }

    /** {@inheritDoc ActionCommandStreamPort.publish} */
    public publish(command: ReminderActionCommand): Promise<void> {
        this.cleanExpiredActions();
        const expiresAt = Date.parse(command.expiresAt);
        if (!Number.isFinite(expiresAt) || expiresAt <= Date.now() || this.closedActions.has(command.commandId)) {
            return Promise.resolve();
        }
        const key = scopeKey(command.deviceId, command.reminderTriggerId);
        const previous = this.actionScopes.get(command.commandId);
        if (previous !== undefined) this.removeActionFromScope(command.commandId, previous.key);
        this.actionScopes.set(command.commandId, { expiresAt, key });
        let actions = this.scopeActions.get(key);
        if (actions === undefined) {
            actions = new Set<ActionId>();
            this.scopeActions.set(key, actions);
        }
        actions.add(command.commandId);
        for (const subscription of this.subscriptions) {
            if (subscription.key === key && expiresAt <= subscription.expiresAt) subscription.push(command);
        }
        return Promise.resolve();
    }

    /** {@inheritDoc ActionCommandStreamPort.subscribe} */
    public subscribe(subscription: ActionStreamSubscription): AsyncIterable<ReminderActionCommand> {
        const live = new HubSubscription(
            subscription,
            this.maxQueueSize,
            (current) => this.subscriptions.delete(current),
            () => this.notifyOverflow(subscription),
        );
        if (live.closed) return live;
        const scopeSubscriptions = [...this.subscriptions].filter((current) => current.key === live.key).length;
        const deviceSubscriptions = [...this.subscriptions].filter(
            (current) => current.deviceId === subscription.deviceId,
        ).length;
        if (
            this.subscriptions.size >= this.maxSubscriptions ||
            deviceSubscriptions >= this.maxSubscriptionsPerDevice ||
            scopeSubscriptions >= this.maxSubscriptionsPerScope
        ) {
            live.finish();
            throw new ImGatewayError('resource_exhausted', 'SSE subscription capacity was reached', true);
        }
        if (!live.closed) this.subscriptions.add(live);
        return live;
    }

    /** {@inheritDoc ActionCommandStreamPort.close} */
    public close(actionId: ActionId, scope: ActionStreamCloseScope): Promise<void> {
        this.cleanExpiredActions();
        const actionScope = this.actionScopes.get(actionId);
        if (actionScope !== undefined) {
            this.actionScopes.delete(actionId);
            this.removeActionFromScope(actionId, actionScope.key);
        }
        const expiresAt = Date.parse(scope.expiresAt);
        this.closedActions.set(actionId, Number.isFinite(expiresAt) ? expiresAt : Date.now() + 60_000);
        const key = scopeKey(scope.deviceId, scope.reminderTriggerId);
        const pendingActions = this.scopeActions.get(key);
        if (pendingActions === undefined || pendingActions.size === 0) {
            for (const subscription of [...this.subscriptions]) {
                if (subscription.key === key) subscription.finish();
            }
        }
        return Promise.resolve();
    }

    private removeActionFromScope(actionId: ActionId, key: string): void {
        const actions = this.scopeActions.get(key);
        if (actions === undefined) return;
        actions.delete(actionId);
        if (actions.size === 0) this.scopeActions.delete(key);
    }

    private cleanExpiredActions(): void {
        const now = Date.now();
        for (const [actionId, scope] of this.actionScopes) {
            if (scope.expiresAt <= now) {
                this.actionScopes.delete(actionId);
                this.removeActionFromScope(actionId, scope.key);
            }
        }
        for (const [actionId, expiresAt] of this.closedActions) {
            if (expiresAt <= now) this.closedActions.delete(actionId);
        }
    }

    private notifyOverflow(subscription: ActionStreamSubscription): void {
        try {
            this.options.subscriptionOverflowed?.({
                deviceId: subscription.deviceId,
                reminderTriggerId: subscription.reminderTriggerId,
                expiresAt: subscription.expiresAt,
            });
        } catch {
            // Observability failures must not preserve an overflowing subscriber.
        }
    }
}

class HubSubscription implements AsyncIterableIterator<ReminderActionCommand> {
    public readonly deviceId: string;

    public readonly key: string;

    public readonly expiresAt: number;

    public closed = false;

    private readonly queue: ReminderActionCommand[] = [];

    private readonly waiters: ((result: IteratorResult<ReminderActionCommand>) => void)[] = [];

    private expiresTimer: ReturnType<typeof setTimeout> | undefined;

    private readonly abortHandler: (() => void) | undefined;

    public constructor(
        private readonly subscription: ActionStreamSubscription,
        private readonly maxQueueSize: number,
        private readonly onFinish: (subscription: HubSubscription) => void,
        private readonly onOverflow: () => void,
    ) {
        this.deviceId = subscription.deviceId;
        this.key = scopeKey(subscription.deviceId, subscription.reminderTriggerId);
        this.expiresAt = Date.parse(subscription.expiresAt);
        if (!Number.isFinite(this.expiresAt) || this.expiresAt <= Date.now() || subscription.signal?.aborted) {
            this.closed = true;
            this.expiresTimer = undefined;
            this.abortHandler = undefined;
            return;
        }
        this.scheduleExpiry();
        if (subscription.signal === undefined) {
            this.abortHandler = undefined;
        } else {
            this.abortHandler = () => this.finish();
            subscription.signal.addEventListener('abort', this.abortHandler, { once: true });
        }
    }

    public [Symbol.asyncIterator](): AsyncIterableIterator<ReminderActionCommand> {
        return this;
    }

    public next(): Promise<IteratorResult<ReminderActionCommand>> {
        const queued = this.queue.shift();
        if (queued !== undefined) return Promise.resolve({ done: false, value: queued });
        if (this.closed) return Promise.resolve({ done: true, value: undefined });
        return new Promise((resolve) => this.waiters.push(resolve));
    }

    public return(): Promise<IteratorResult<ReminderActionCommand>> {
        this.finish();
        return Promise.resolve({ done: true, value: undefined });
    }

    public push(command: ReminderActionCommand): void {
        if (this.closed) return;
        const waiter = this.waiters.shift();
        if (waiter === undefined && this.queue.length >= this.maxQueueSize) {
            this.onOverflow();
            this.finish();
        } else if (waiter === undefined) this.queue.push(command);
        else waiter({ done: false, value: command });
    }

    public finish(): void {
        if (this.closed) return;
        this.closed = true;
        this.queue.length = 0;
        if (this.expiresTimer !== undefined) clearTimeout(this.expiresTimer);
        if (this.abortHandler !== undefined) {
            this.subscription.signal?.removeEventListener('abort', this.abortHandler);
        }
        for (const resolve of this.waiters.splice(0)) resolve({ done: true, value: undefined });
        this.onFinish(this);
    }

    private scheduleExpiry(): void {
        const delay = Math.min(MAX_TIMER_DELAY_MS, Math.max(0, this.expiresAt - Date.now()));
        this.expiresTimer = setTimeout(() => {
            this.expiresTimer = undefined;
            if (this.expiresAt <= Date.now()) this.finish();
            else this.scheduleExpiry();
        }, delay);
        if (delay > 1_000) this.expiresTimer.unref();
    }
}

function scopeKey(deviceId: string, reminderTriggerId: string): string {
    return `${deviceId.length}:${deviceId}${reminderTriggerId}`;
}

function positiveInteger(value: number): number {
    if (!Number.isSafeInteger(value) || value <= 0) throw new Error('SSE capacity must be a positive integer');
    return value;
}
