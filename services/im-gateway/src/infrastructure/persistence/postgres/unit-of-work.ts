import type {
    ActionRepository,
    BindingRepository,
    ChannelAccountRepository,
    DeliveryRepository,
    DeviceRepository,
    IdentityRepository,
    ImUnitOfWorkContext,
    InboundEventRepository,
    IntentSubmissionRepository,
    OutboxRepository,
    PairingSessionRepository,
    ReminderActionFactRepository,
} from '../../../ports/repositories.js';
import { PostgresActionRepository } from './action-repository.js';
import { PostgresBindingRepository } from './binding-repository.js';
import { PostgresChannelAccountRepository } from './channel-account-repository.js';
import { PostgresDeliveryRepository } from './delivery-repository.js';
import { PostgresDeviceRepository } from './device-repository.js';
import { PostgresIdentityRepository } from './identity-repository.js';
import { PostgresInboundEventRepository } from './inbound-event-repository.js';
import { PostgresIntentSubmissionRepository } from './intent-submission-repository.js';
import { PostgresOutboxRepository } from './outbox-repository.js';
import { PostgresPairingSessionRepository } from './pairing-session-repository.js';
import { PostgresReminderActionFactRepository } from './reminder-action-fact-repository.js';
import type { SqlExecutor } from './sql.js';

/** 同一事务内可用的全部 PostgreSQL 仓储。 */
export class PostgresUnitOfWorkContext implements ImUnitOfWorkContext {
    /** 注册设备仓储。 */
    public readonly devices: DeviceRepository;
    /** 渠道账号仓储。 */
    public readonly channelAccounts: ChannelAccountRepository;
    /** 配对会话仓储。 */
    public readonly pairingSessions: PairingSessionRepository;
    /** 外部身份仓储。 */
    public readonly identities: IdentityRepository;
    /** 绑定仓储。 */
    public readonly bindings: BindingRepository;
    /** 入站事件仓储。 */
    public readonly inboundEvents: InboundEventRepository;
    /** 受理记录仓储。 */
    public readonly intentSubmissions: IntentSubmissionRepository;
    /** 投递仓储。 */
    public readonly deliveries: DeliveryRepository;
    /** 动作仓储。 */
    public readonly actions: ActionRepository;
    /** 设备语音动作事实仓储。 */
    public readonly reminderActionFacts: ReminderActionFactRepository;
    /** 事务性发件箱仓储。 */
    public readonly outbox: OutboxRepository;

    /** @param executor 绑定到当前事务的客户端。 */
    public constructor(executor: SqlExecutor) {
        this.devices = new PostgresDeviceRepository(executor);
        this.channelAccounts = new PostgresChannelAccountRepository(executor);
        this.pairingSessions = new PostgresPairingSessionRepository(executor);
        this.identities = new PostgresIdentityRepository(executor);
        this.bindings = new PostgresBindingRepository(executor);
        this.inboundEvents = new PostgresInboundEventRepository(executor);
        this.intentSubmissions = new PostgresIntentSubmissionRepository(executor);
        this.deliveries = new PostgresDeliveryRepository(executor);
        this.actions = new PostgresActionRepository(executor);
        this.reminderActionFacts = new PostgresReminderActionFactRepository(executor);
        this.outbox = new PostgresOutboxRepository(executor);
    }
}
