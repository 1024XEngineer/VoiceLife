import type {
    ActionId,
    BindingId,
    ChannelAccountId,
    DeliveryId,
    DeviceId,
    EventId,
    ExternalIdentityId,
    InboundEventId,
    OperationId,
    PairingSessionId,
    ReminderTriggerId,
    UserId,
} from '../../../contracts/ids.js';
import type {
    ChannelAccount,
    Delivery,
    DeliveryAttempt,
    DeliveryReceipt,
    ExternalIdentity,
    ImAction,
    ImBinding,
    ImOutboxEvent,
    InboundEventRecord,
    IntentSubmissionRecord,
    PairingSession,
} from '../../../domain/models.js';
import type {
    ActionRepository,
    BindingRepository,
    ChannelAccountRepository,
    DeliveryRepository,
    IdentityRepository,
    ImUnitOfWorkContext,
    InboundEventRepository,
    IntentSubmissionRepository,
    OutboxRepository,
    PairingSessionRepository,
} from '../../../ports/repositories.js';
import type { IsoDateTime } from '../../../shared/types.js';
import {
    mapAction,
    mapBinding,
    mapChannelAccount,
    mapDelivery,
    mapDeliveryAttempt,
    mapDeliveryReceipt,
    mapExternalIdentity,
    mapInboundEvent,
    mapIntentSubmission,
    mapPairingSession,
    type DbRow,
} from './mappers.js';

/** 可执行参数化 SQL 的最小接口，由连接池或事务客户端实现。 */
export interface SqlExecutor {
    /**
     * 执行一条参数化查询。
     * @param text 参数化 SQL。
     * @param values 绑定参数。
     * @returns 查询结果行。
     */
    query(text: string, values?: readonly unknown[]): Promise<{ rows: readonly DbRow[] }>;
}

/**
 * 按冲突列做整行 upsert：冲突时把非冲突列替换为本次写入值。
 * @param executor SQL 执行器。
 * @param table 表名。
 * @param columns 列名。
 * @param row 行数据。
 * @param conflict 冲突列。
 */
async function upsert(
    executor: SqlExecutor,
    table: string,
    columns: readonly string[],
    row: readonly unknown[],
    conflict: readonly string[],
): Promise<void> {
    const quoted = columns.map((column) => `"${column}"`).join(', ');
    const placeholders = columns.map((_, index) => `$${index + 1}`).join(', ');
    const conflictTarget = conflict.map((column) => `"${column}"`).join(', ');
    const updates = columns
        .filter((column) => !conflict.includes(column))
        .map((column) => `"${column}" = EXCLUDED."${column}"`)
        .join(', ');
    await executor.query(
        `INSERT INTO "${table}" (${quoted}) VALUES (${placeholders}) ON CONFLICT (${conflictTarget}) DO UPDATE SET ${updates}`,
        row,
    );
}

/** 将 JSON 值序列化为 jsonb 参数；pg 会把 JS 数组特殊处理为数组字面量，必须显式序列化。 */
function toJson(value: unknown): string | null {
    return value === undefined || value === null ? null : JSON.stringify(value);
}

/**
 * 执行查询并返回首行，无结果时返回 undefined。
 * @param executor SQL 执行器。
 * @param sql SQL 语句。
 * @param params 参数。
 * @returns 查询结果行。
 */
async function queryOne(executor: SqlExecutor, sql: string, params: readonly unknown[]): Promise<DbRow | undefined> {
    const { rows } = await executor.query(sql, params);
    return rows[0];
}

const CHANNEL_COLUMNS = [
    'id',
    'platform',
    'tenant_external_id',
    'koishi_bot_id',
    'credential_ref',
    'connection_mode',
    'capability_config',
    'status',
    'created_at',
    'updated_at',
] as const;

const PAIRING_COLUMNS = [
    'id',
    'display_code_hash',
    'user_id',
    'device_id',
    'allowed_platforms',
    'status',
    'expires_at',
    'created_at',
    'confirmed_at',
] as const;

const IDENTITY_COLUMNS = [
    'id',
    'channel_account_id',
    'external_user_id_ciphertext',
    'external_user_id_hash',
    'display_name',
    'status',
    'created_at',
    'updated_at',
] as const;

const BINDING_COLUMNS = [
    'id',
    'user_id',
    'device_id',
    'external_identity_id',
    'priority',
    'status',
    'bound_at',
    'unbound_at',
    'revoked_at',
] as const;

const INBOUND_COLUMNS = [
    'id',
    'channel_account_id',
    'external_event_id',
    'event_type',
    'payload',
    'status',
    'occurred_at',
    'received_at',
] as const;

const INTENT_COLUMNS = ['business_event_id', 'kind', 'request_fingerprint', 'submission', 'created_at'] as const;

const DELIVERY_COLUMNS = [
    'id',
    'business_event_id',
    'correlation_id',
    'binding_id',
    'channel_account_id',
    'kind',
    'semantic_payload',
    'presentation_type',
    'status',
    'external_message_id',
    'expires_at',
    'last_error_code',
    'created_at',
    'updated_at',
] as const;

const ATTEMPT_COLUMNS = [
    'id',
    'delivery_id',
    'attempt_no',
    'request_id',
    'rendered_payload',
    'status',
    'platform_message_id',
    'error_code',
    'started_at',
    'completed_at',
] as const;

const RECEIPT_COLUMNS = [
    'id',
    'delivery_id',
    'attempt_id',
    'stage',
    'dedupe_key',
    'external_event_id',
    'detail',
    'occurred_at',
    'received_at',
] as const;

const ACTION_COLUMNS = [
    'id',
    'operation_id',
    'correlation_id',
    'delivery_id',
    'actor_binding_id',
    'device_id',
    'reminder_trigger_id',
    'action_type',
    'action_params',
    'action_key_hash',
    'expected_identity_id',
    'actual_identity_id',
    'status',
    'dispatched_at',
    'result',
    'expires_at',
    'created_at',
    'updated_at',
] as const;

const OUTBOX_COLUMNS = [
    'id',
    'event_type',
    'aggregate_id',
    'payload',
    'status',
    'attempts',
    'available_at',
    'created_at',
    'published_at',
] as const;

/** 渠道账号的 PostgreSQL 实现。 */
export class PostgresChannelAccountRepository implements ChannelAccountRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc ChannelAccountRepository.findById} */
    public async findById(id: ChannelAccountId): Promise<ChannelAccount | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_channel_accounts WHERE id = $1', [id]);
        return row === undefined ? undefined : mapChannelAccount(row);
    }

    /** {@inheritDoc ChannelAccountRepository.save} */
    public async save(account: ChannelAccount): Promise<void> {
        await upsert(
            this.executor,
            'im_channel_accounts',
            CHANNEL_COLUMNS,
            [
                account.id,
                account.platform,
                account.tenantExternalId,
                account.koishiBotId,
                account.credentialRef,
                account.connectionMode,
                toJson(account.capabilityConfig),
                account.status,
                account.createdAt,
                account.updatedAt,
            ],
            ['id'],
        );
    }
}

/** 配对会话的 PostgreSQL 实现。 */
export class PostgresPairingSessionRepository implements PairingSessionRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc PairingSessionRepository.findById} */
    public async findById(id: PairingSessionId): Promise<PairingSession | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_pairing_sessions WHERE id = $1', [id]);
        return row === undefined ? undefined : mapPairingSession(row);
    }

    /** {@inheritDoc PairingSessionRepository.findPendingByDisplayCodeHash} */
    public async findPendingByDisplayCodeHash(hash: string): Promise<PairingSession | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_pairing_sessions WHERE display_code_hash = $1 AND status = $2 ORDER BY created_at ASC, id ASC LIMIT 1',
            [hash, 'pending'],
        );
        return row === undefined ? undefined : mapPairingSession(row);
    }

    /** {@inheritDoc PairingSessionRepository.findExpiredPairingSessions} */
    public async findExpiredPairingSessions(now: IsoDateTime): Promise<readonly PairingSession[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_pairing_sessions WHERE status = $1 AND expires_at <= $2 ORDER BY created_at ASC, id ASC',
            ['pending', now],
        );
        return rows.map(mapPairingSession);
    }

    /** {@inheritDoc PairingSessionRepository.save} */
    public async save(session: PairingSession): Promise<void> {
        await upsert(
            this.executor,
            'im_pairing_sessions',
            PAIRING_COLUMNS,
            [
                session.id,
                session.displayCodeHash,
                session.userId ?? null,
                session.deviceId,
                toJson(session.allowedPlatforms),
                session.status,
                session.expiresAt,
                session.createdAt,
                session.confirmedAt ?? null,
            ],
            ['id'],
        );
    }
}

/** 受保护外部身份的 PostgreSQL 实现。 */
export class PostgresIdentityRepository implements IdentityRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc IdentityRepository.findById} */
    public async findById(id: ExternalIdentityId): Promise<ExternalIdentity | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_external_identities WHERE id = $1', [id]);
        return row === undefined ? undefined : mapExternalIdentity(row);
    }

    /** {@inheritDoc IdentityRepository.findByChannelAndHash} */
    public async findByChannelAndHash(
        channelAccountId: ChannelAccountId,
        externalUserIdHash: string,
    ): Promise<ExternalIdentity | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_external_identities WHERE channel_account_id = $1 AND external_user_id_hash = $2 LIMIT 1',
            [channelAccountId, externalUserIdHash],
        );
        return row === undefined ? undefined : mapExternalIdentity(row);
    }

    /** {@inheritDoc IdentityRepository.save} */
    public async save(identity: ExternalIdentity): Promise<void> {
        await upsert(
            this.executor,
            'im_external_identities',
            IDENTITY_COLUMNS,
            [
                identity.id,
                identity.channelAccountId,
                identity.externalUserIdCiphertext,
                identity.externalUserIdHash,
                identity.displayName ?? null,
                identity.status,
                identity.createdAt,
                identity.updatedAt,
            ],
            ['id'],
        );
    }
}

/** 绑定关系的 PostgreSQL 实现。 */
export class PostgresBindingRepository implements BindingRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc BindingRepository.findById} */
    public async findById(id: BindingId): Promise<ImBinding | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_bindings WHERE id = $1', [id]);
        return row === undefined ? undefined : mapBinding(row);
    }

    /** {@inheritDoc BindingRepository.listActiveByUser} */
    public async listActiveByUser(userId: UserId): Promise<readonly ImBinding[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_bindings WHERE user_id = $1 AND status = $2 ORDER BY priority ASC',
            [userId, 'active'],
        );
        return rows.map(mapBinding);
    }

    /** {@inheritDoc BindingRepository.findActiveByDevice} */
    public async findActiveByDevice(deviceId: DeviceId): Promise<readonly ImBinding[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_bindings WHERE device_id = $1 AND status = $2 ORDER BY priority ASC',
            [deviceId, 'active'],
        );
        return rows.map(mapBinding);
    }

    /** {@inheritDoc BindingRepository.findActiveByIdentity} */
    public async findActiveByIdentity(externalIdentityId: ExternalIdentityId): Promise<ImBinding | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_bindings WHERE external_identity_id = $1 AND status = $2 ORDER BY priority ASC LIMIT 1',
            [externalIdentityId, 'active'],
        );
        return row === undefined ? undefined : mapBinding(row);
    }

    /** {@inheritDoc BindingRepository.save} */
    public async save(binding: ImBinding): Promise<void> {
        await upsert(
            this.executor,
            'im_bindings',
            BINDING_COLUMNS,
            [
                binding.id,
                binding.userId,
                binding.deviceId ?? null,
                binding.externalIdentityId,
                binding.priority,
                binding.status,
                binding.boundAt,
                binding.unboundAt ?? null,
                binding.revokedAt ?? null,
            ],
            ['id'],
        );
    }
}

/** 规范化入站事件的 PostgreSQL 实现。 */
export class PostgresInboundEventRepository implements InboundEventRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc InboundEventRepository.findById} */
    public async findById(id: InboundEventId): Promise<InboundEventRecord | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_inbound_events WHERE id = $1', [id]);
        return row === undefined ? undefined : mapInboundEvent(row);
    }

    /** {@inheritDoc InboundEventRepository.findByExternalEvent} */
    public async findByExternalEvent(
        channelAccountId: ChannelAccountId,
        externalEventId: string,
    ): Promise<InboundEventRecord | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_inbound_events WHERE channel_account_id = $1 AND external_event_id = $2 LIMIT 1',
            [channelAccountId, externalEventId],
        );
        return row === undefined ? undefined : mapInboundEvent(row);
    }

    /** {@inheritDoc InboundEventRepository.save} */
    public async save(event: InboundEventRecord): Promise<void> {
        await upsert(
            this.executor,
            'im_inbound_events',
            INBOUND_COLUMNS,
            [
                event.id,
                event.channelAccountId,
                event.externalEventId,
                event.eventType,
                toJson(event.payload),
                event.status,
                event.occurredAt,
                event.receivedAt,
            ],
            ['channel_account_id', 'external_event_id'],
        );
    }
}

/** 请求级幂等记录的 PostgreSQL 实现。 */
export class PostgresIntentSubmissionRepository implements IntentSubmissionRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc IntentSubmissionRepository.findByBusinessKey} */
    public async findByBusinessKey(
        businessEventId: EventId,
        kind: IntentSubmissionRecord['kind'],
    ): Promise<IntentSubmissionRecord | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_intent_submissions WHERE business_event_id = $1 AND kind = $2 LIMIT 1',
            [businessEventId, kind],
        );
        return row === undefined ? undefined : mapIntentSubmission(row);
    }

    /** {@inheritDoc IntentSubmissionRepository.save} */
    public async save(record: IntentSubmissionRecord): Promise<void> {
        await upsert(
            this.executor,
            'im_intent_submissions',
            INTENT_COLUMNS,
            [
                record.businessEventId,
                record.kind,
                record.requestFingerprint,
                toJson(record.submission),
                record.createdAt,
            ],
            ['business_event_id', 'kind'],
        );
    }
}

/** 投递聚合的 PostgreSQL 实现。 */
export class PostgresDeliveryRepository implements DeliveryRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc DeliveryRepository.findById} */
    public async findById(id: DeliveryId): Promise<Delivery | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_deliveries WHERE id = $1', [id]);
        return row === undefined ? undefined : mapDelivery(row);
    }

    /** {@inheritDoc DeliveryRepository.findByExternalMessage} */
    public async findByExternalMessage(
        channelAccountId: ChannelAccountId,
        externalMessageId: string,
    ): Promise<Delivery | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_deliveries WHERE channel_account_id = $1 AND external_message_id = $2 LIMIT 1',
            [channelAccountId, externalMessageId],
        );
        if (row !== undefined) return mapDelivery(row);
        const viaAttempt = await queryOne(
            this.executor,
            `SELECT d.* FROM im_delivery_attempts a JOIN im_deliveries d ON d.id = a.delivery_id
             WHERE a.platform_message_id = $1 AND d.channel_account_id = $2 LIMIT 1`,
            [externalMessageId, channelAccountId],
        );
        return viaAttempt === undefined ? undefined : mapDelivery(viaAttempt);
    }

    /** {@inheritDoc DeliveryRepository.findActiveActionWindow} */
    public async findActiveActionWindow(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<Delivery | undefined> {
        const row = await queryOne(
            this.executor,
            `SELECT * FROM im_deliveries
             WHERE expires_at > $1
               AND semantic_payload ->> 'reminderType' = 'strong'
               AND semantic_payload ->> 'reminderTriggerId' = $2
               AND semantic_payload -> 'recipient' ->> 'deviceId' = $3
             ORDER BY created_at ASC, id ASC LIMIT 1`,
            [now, reminderTriggerId, deviceId],
        );
        return row === undefined ? undefined : mapDelivery(row);
    }

    /** {@inheritDoc DeliveryRepository.findByBusinessKey} */
    public async findByBusinessKey(
        businessEventId: EventId,
        bindingId: BindingId,
        kind: Delivery['kind'],
    ): Promise<Delivery | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_deliveries WHERE business_event_id = $1 AND binding_id = $2 AND kind = $3 LIMIT 1',
            [businessEventId, bindingId, kind],
        );
        return row === undefined ? undefined : mapDelivery(row);
    }

    /** {@inheritDoc DeliveryRepository.save} */
    public async save(delivery: Delivery): Promise<void> {
        await upsert(
            this.executor,
            'im_deliveries',
            DELIVERY_COLUMNS,
            [
                delivery.id,
                delivery.businessEventId,
                delivery.correlationId,
                delivery.bindingId,
                delivery.channelAccountId,
                delivery.kind,
                toJson(delivery.semanticPayload),
                delivery.presentationType,
                delivery.status,
                delivery.externalMessageId ?? null,
                delivery.expiresAt ?? null,
                delivery.lastErrorCode ?? null,
                delivery.createdAt,
                delivery.updatedAt,
            ],
            ['id'],
        );
    }

    /** {@inheritDoc DeliveryRepository.findAttempt} */
    public async findAttempt(deliveryId: DeliveryId, attemptNo: number): Promise<DeliveryAttempt | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_delivery_attempts WHERE delivery_id = $1 AND attempt_no = $2 LIMIT 1',
            [deliveryId, attemptNo],
        );
        return row === undefined ? undefined : mapDeliveryAttempt(row);
    }

    /** {@inheritDoc DeliveryRepository.nextAttemptNo} */
    public async nextAttemptNo(deliveryId: DeliveryId): Promise<number> {
        const row = await queryOne(
            this.executor,
            'SELECT COALESCE(MAX(attempt_no), 0) + 1 AS next_no FROM im_delivery_attempts WHERE delivery_id = $1',
            [deliveryId],
        );
        return (row?.next_no as number) ?? 1;
    }

    /** {@inheritDoc DeliveryRepository.listAttempts} */
    public async listAttempts(deliveryId: DeliveryId): Promise<readonly DeliveryAttempt[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_delivery_attempts WHERE delivery_id = $1 ORDER BY attempt_no ASC',
            [deliveryId],
        );
        return rows.map(mapDeliveryAttempt);
    }

    /** {@inheritDoc DeliveryRepository.saveAttempt} */
    public async saveAttempt(attempt: DeliveryAttempt): Promise<void> {
        await upsert(
            this.executor,
            'im_delivery_attempts',
            ATTEMPT_COLUMNS,
            [
                attempt.id,
                attempt.deliveryId,
                attempt.attemptNo,
                attempt.requestId,
                toJson(attempt.renderedPayload),
                attempt.status,
                attempt.platformMessageId ?? null,
                attempt.errorCode ?? null,
                attempt.startedAt,
                attempt.completedAt ?? null,
            ],
            ['id'],
        );
    }

    /** {@inheritDoc DeliveryRepository.findReceiptByDedupeKey} */
    public async findReceiptByDedupeKey(dedupeKey: string): Promise<DeliveryReceipt | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_delivery_receipts WHERE dedupe_key = $1 LIMIT 1', [
            dedupeKey,
        ]);
        return row === undefined ? undefined : mapDeliveryReceipt(row);
    }

    /** {@inheritDoc DeliveryRepository.listReceipts} */
    public async listReceipts(deliveryId: DeliveryId): Promise<readonly DeliveryReceipt[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_delivery_receipts WHERE delivery_id = $1 ORDER BY occurred_at ASC, id ASC',
            [deliveryId],
        );
        return rows.map(mapDeliveryReceipt);
    }

    /** {@inheritDoc DeliveryRepository.saveReceipt} */
    public async saveReceipt(receipt: DeliveryReceipt): Promise<void> {
        await upsert(
            this.executor,
            'im_delivery_receipts',
            RECEIPT_COLUMNS,
            [
                receipt.id,
                receipt.deliveryId,
                receipt.attemptId ?? null,
                receipt.stage,
                receipt.dedupeKey,
                receipt.externalEventId ?? null,
                toJson(receipt.detail),
                receipt.occurredAt,
                receipt.receivedAt,
            ],
            ['id'],
        );
    }
}

/** 提醒动作的 PostgreSQL 实现。 */
export class PostgresActionRepository implements ActionRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc ActionRepository.findById} */
    public async findById(id: ActionId): Promise<ImAction | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_actions WHERE id = $1', [id]);
        return row === undefined ? undefined : mapAction(row);
    }

    /** {@inheritDoc ActionRepository.findByOperationId} */
    public async findByOperationId(operationId: OperationId): Promise<ImAction | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_actions WHERE operation_id = $1 LIMIT 1', [
            operationId,
        ]);
        return row === undefined ? undefined : mapAction(row);
    }

    /** {@inheritDoc ActionRepository.findByActionKeyHash} */
    public async findByActionKeyHash(actionKeyHash: string): Promise<ImAction | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_actions WHERE action_key_hash = $1 LIMIT 1', [
            actionKeyHash,
        ]);
        return row === undefined ? undefined : mapAction(row);
    }

    /** {@inheritDoc ActionRepository.findPendingByDeviceAndTrigger} */
    public async findPendingByDeviceAndTrigger(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<readonly ImAction[]> {
        const { rows } = await this.executor.query(
            `SELECT * FROM im_actions
             WHERE device_id = $1 AND reminder_trigger_id = $2 AND expires_at > $3
               AND status IN ('pending', 'dispatched', 'processing')
             ORDER BY created_at ASC, id ASC`,
            [deviceId, reminderTriggerId, now],
        );
        return rows.map(mapAction);
    }

    /** {@inheritDoc ActionRepository.findExpiredActions} */
    public async findExpiredActions(now: IsoDateTime): Promise<readonly ImAction[]> {
        const { rows } = await this.executor.query(
            `SELECT * FROM im_actions
             WHERE expires_at <= $1 AND status IN ('pending', 'dispatched', 'processing')
             ORDER BY created_at ASC, id ASC`,
            [now],
        );
        return rows.map(mapAction);
    }

    /** {@inheritDoc ActionRepository.save} */
    public async save(action: ImAction): Promise<void> {
        await upsert(
            this.executor,
            'im_actions',
            ACTION_COLUMNS,
            [
                action.id,
                action.operationId,
                action.correlationId,
                action.deliveryId,
                action.actorBindingId,
                action.deviceId,
                action.reminderTriggerId,
                action.actionType,
                toJson(action.actionParams),
                action.actionKeyHash,
                action.expectedIdentityId,
                action.actualIdentityId ?? null,
                action.status,
                action.dispatchedAt ?? null,
                toJson(action.result),
                action.expiresAt,
                action.createdAt,
                action.updatedAt,
            ],
            ['id'],
        );
    }
}

/** 事务性发件箱的 PostgreSQL 实现。 */
export class PostgresOutboxRepository implements OutboxRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc OutboxRepository.append} */
    public async append(event: ImOutboxEvent): Promise<void> {
        await this.executor.query(
            `INSERT INTO im_outbox_events (${OUTBOX_COLUMNS.map((column) => `"${column}"`).join(', ')})
             VALUES (${OUTBOX_COLUMNS.map((_, index) => `$${index + 1}`).join(', ')})`,
            [
                event.id,
                event.eventType,
                event.aggregateId,
                toJson(event.payload),
                event.status,
                event.attempts,
                event.availableAt,
                event.createdAt,
                event.publishedAt ?? null,
            ],
        );
    }
}

/** 同一事务内可用的全部 PostgreSQL 仓储。 */
export class PostgresUnitOfWorkContext implements ImUnitOfWorkContext {
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
    /** 事务性发件箱仓储。 */
    public readonly outbox: OutboxRepository;

    /** @param executor 绑定到当前事务的客户端。 */
    public constructor(executor: SqlExecutor) {
        this.channelAccounts = new PostgresChannelAccountRepository(executor);
        this.pairingSessions = new PostgresPairingSessionRepository(executor);
        this.identities = new PostgresIdentityRepository(executor);
        this.bindings = new PostgresBindingRepository(executor);
        this.inboundEvents = new PostgresInboundEventRepository(executor);
        this.intentSubmissions = new PostgresIntentSubmissionRepository(executor);
        this.deliveries = new PostgresDeliveryRepository(executor);
        this.actions = new PostgresActionRepository(executor);
        this.outbox = new PostgresOutboxRepository(executor);
    }
}
