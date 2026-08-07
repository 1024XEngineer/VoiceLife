import type { ActionId } from '../../contracts/ids.js';
import type { ReminderActionCommand } from '../../contracts/device-gateway.js';
import type {
    ActionCommandStreamPort,
    ActionStreamCloseScope,
    ActionStreamSubscription,
} from '../../ports/external.js';

const MAX_TIMER_DELAY_MS = 2_147_483_647;

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
        const live = new HubSubscription(subscription, (current) => this.subscriptions.delete(current));
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
}

class HubSubscription implements AsyncIterableIterator<ReminderActionCommand> {
    public readonly key: string;

    public readonly expiresAt: number;

    public closed = false;

    private readonly queue: ReminderActionCommand[] = [];

    private readonly waiters: ((result: IteratorResult<ReminderActionCommand>) => void)[] = [];

    private expiresTimer: ReturnType<typeof setTimeout> | undefined;

    private readonly abortHandler: (() => void) | undefined;

    public constructor(
        private readonly subscription: ActionStreamSubscription,
        private readonly onFinish: (subscription: HubSubscription) => void,
    ) {
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
        if (waiter === undefined) this.queue.push(command);
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
