import { queryOne, type SqlExecutor } from './sql.js';

/** 当前 schema 版本号；低于该版本的库会在 migrate() 时逐版本升级。 */
export const SCHEMA_VERSION = 10;

/** 迁移版本表：version 行与对应 DDL 在同一事务内写入，保证原子可见。 */
const SCHEMA_MIGRATIONS_TABLE = 'im_schema_migrations';

/** 跨实例串行迁移的会话级咨询锁键，必须为稳定常量，避免并发迁移交错执行。 */
const MIGRATION_LOCK_KEY = 727271001288;

/** IM Gateway 持久化表清单，按外键依赖顺序排列，供清空与诊断使用。 */
export const IM_TABLES = [
    'im_devices',
    'im_channel_accounts',
    'im_pairing_sessions',
    'im_external_identities',
    'im_bindings',
    'im_inbound_events',
    'im_intent_submissions',
    'im_deliveries',
    'im_delivery_attempts',
    'im_delivery_receipts',
    'im_actions',
    'im_reminder_action_facts',
    'im_outbox_events',
] as const;

/** v1 幂等迁移脚本：全部使用 IF NOT EXISTS，可重复执行。 */
const V1_STATEMENTS: readonly string[] = [
    `CREATE TABLE IF NOT EXISTS im_channel_accounts (
        id text PRIMARY KEY,
        platform text NOT NULL,
        tenant_external_id text NOT NULL,
        koishi_bot_id text NOT NULL,
        credential_ref text NOT NULL,
        connection_mode text NOT NULL,
        capability_config jsonb,
        status text NOT NULL,
        created_at timestamptz NOT NULL,
        updated_at timestamptz NOT NULL
    )`,
    `CREATE TABLE IF NOT EXISTS im_pairing_sessions (
        id text PRIMARY KEY,
        display_code_hash text NOT NULL,
        user_id text,
        device_id text NOT NULL,
        allowed_platforms jsonb,
        status text NOT NULL,
        expires_at timestamptz NOT NULL,
        created_at timestamptz NOT NULL,
        confirmed_at timestamptz
    )`,
    `CREATE INDEX IF NOT EXISTS im_pairing_sessions_display_code_hash_idx
        ON im_pairing_sessions (display_code_hash)`,
    `CREATE INDEX IF NOT EXISTS im_pairing_sessions_expires_at_idx
        ON im_pairing_sessions (expires_at)`,
    `CREATE TABLE IF NOT EXISTS im_external_identities (
        id text PRIMARY KEY,
        channel_account_id text NOT NULL,
        external_user_id_ciphertext text NOT NULL,
        external_user_id_hash text NOT NULL,
        display_name text,
        status text NOT NULL,
        created_at timestamptz NOT NULL,
        updated_at timestamptz NOT NULL,
        UNIQUE (channel_account_id, external_user_id_hash)
    )`,
    `CREATE TABLE IF NOT EXISTS im_bindings (
        id text PRIMARY KEY,
        user_id text NOT NULL,
        device_id text,
        external_identity_id text NOT NULL,
        priority integer NOT NULL,
        status text NOT NULL,
        bound_at timestamptz NOT NULL,
        unbound_at timestamptz,
        revoked_at timestamptz
    )`,
    `CREATE INDEX IF NOT EXISTS im_bindings_user_id_idx ON im_bindings (user_id)`,
    `CREATE INDEX IF NOT EXISTS im_bindings_device_id_idx ON im_bindings (device_id)`,
    `CREATE INDEX IF NOT EXISTS im_bindings_external_identity_id_idx
        ON im_bindings (external_identity_id)`,
    `CREATE TABLE IF NOT EXISTS im_inbound_events (
        id text PRIMARY KEY,
        channel_account_id text NOT NULL,
        external_event_id text NOT NULL,
        event_type text NOT NULL,
        payload jsonb NOT NULL,
        status text NOT NULL,
        occurred_at timestamptz NOT NULL,
        received_at timestamptz NOT NULL,
        UNIQUE (channel_account_id, external_event_id)
    )`,
    `CREATE TABLE IF NOT EXISTS im_intent_submissions (
        business_event_id text NOT NULL,
        kind text NOT NULL,
        request_fingerprint text NOT NULL,
        submission jsonb NOT NULL,
        created_at timestamptz NOT NULL,
        PRIMARY KEY (business_event_id, kind)
    )`,
    `CREATE TABLE IF NOT EXISTS im_deliveries (
        id text PRIMARY KEY,
        business_event_id text NOT NULL,
        correlation_id text NOT NULL,
        binding_id text NOT NULL,
        channel_account_id text NOT NULL,
        kind text NOT NULL,
        semantic_payload jsonb NOT NULL,
        presentation_type text NOT NULL,
        status text NOT NULL,
        external_message_id text,
        expires_at timestamptz,
        last_error_code text,
        created_at timestamptz NOT NULL,
        updated_at timestamptz NOT NULL,
        UNIQUE (business_event_id, binding_id, kind)
    )`,
    `CREATE INDEX IF NOT EXISTS im_deliveries_channel_external_idx
        ON im_deliveries (channel_account_id, external_message_id)`,
    `CREATE TABLE IF NOT EXISTS im_delivery_attempts (
        id text PRIMARY KEY,
        delivery_id text NOT NULL,
        attempt_no integer NOT NULL,
        request_id text NOT NULL,
        rendered_payload jsonb NOT NULL,
        status text NOT NULL,
        platform_message_id text,
        error_code text,
        started_at timestamptz NOT NULL,
        completed_at timestamptz,
        UNIQUE (delivery_id, attempt_no)
    )`,
    `CREATE INDEX IF NOT EXISTS im_delivery_attempts_platform_message_idx
        ON im_delivery_attempts (platform_message_id)`,
    `CREATE TABLE IF NOT EXISTS im_delivery_receipts (
        id text PRIMARY KEY,
        delivery_id text NOT NULL,
        attempt_id text,
        stage text NOT NULL,
        dedupe_key text NOT NULL,
        external_event_id text,
        detail jsonb,
        occurred_at timestamptz NOT NULL,
        received_at timestamptz NOT NULL,
        UNIQUE (dedupe_key)
    )`,
    `CREATE INDEX IF NOT EXISTS im_delivery_receipts_delivery_id_idx
        ON im_delivery_receipts (delivery_id)`,
    `CREATE TABLE IF NOT EXISTS im_actions (
        id text PRIMARY KEY,
        operation_id text NOT NULL,
        correlation_id text NOT NULL,
        delivery_id text NOT NULL,
        actor_binding_id text NOT NULL,
        device_id text NOT NULL,
        reminder_trigger_id text NOT NULL,
        action_type text NOT NULL,
        action_params jsonb,
        action_key_hash text NOT NULL,
        expected_identity_id text NOT NULL,
        actual_identity_id text,
        status text NOT NULL,
        dispatched_at timestamptz,
        result jsonb,
        expires_at timestamptz NOT NULL,
        created_at timestamptz NOT NULL,
        updated_at timestamptz NOT NULL,
        UNIQUE (operation_id),
        UNIQUE (action_key_hash)
    )`,
    `CREATE INDEX IF NOT EXISTS im_actions_device_trigger_idx
        ON im_actions (device_id, reminder_trigger_id)`,
    `CREATE INDEX IF NOT EXISTS im_actions_expires_at_idx ON im_actions (expires_at)`,
    `CREATE TABLE IF NOT EXISTS im_outbox_events (
        id text PRIMARY KEY,
        event_type text NOT NULL,
        aggregate_id text NOT NULL,
        payload jsonb NOT NULL,
        status text NOT NULL,
        attempts integer NOT NULL,
        available_at timestamptz NOT NULL,
        created_at timestamptz NOT NULL,
        published_at timestamptz
    )`,
    `CREATE INDEX IF NOT EXISTS im_outbox_events_status_available_idx
        ON im_outbox_events (status, available_at)`,
];

/** v2 幂等迁移：为派发领取与所有权隔离补充 claim 列（sending 且 NULL 视为可重领）。 */
const V2_STATEMENTS: readonly string[] = [
    'ALTER TABLE im_deliveries ADD COLUMN IF NOT EXISTS claimed_at timestamptz',
    'ALTER TABLE im_deliveries ADD COLUMN IF NOT EXISTS claim_token text',
];

/** v3 幂等迁移：清理历史重复绑定，并禁止同一用户、设备与外部身份重复处于 active。 */
const V3_STATEMENTS: readonly string[] = [
    `WITH ranked AS (
        SELECT id,
               row_number() OVER (
                   PARTITION BY user_id, device_id, external_identity_id
                   ORDER BY bound_at DESC, id DESC
               ) AS rank
        FROM im_bindings
        WHERE status = 'active' AND device_id IS NOT NULL
    )
    UPDATE im_bindings AS binding
    SET status = 'unbound', unbound_at = now()
    FROM ranked
    WHERE binding.id = ranked.id AND ranked.rank > 1`,
    `CREATE UNIQUE INDEX IF NOT EXISTS im_bindings_active_user_device_identity_uq
        ON im_bindings (user_id, device_id, external_identity_id)
        WHERE status = 'active' AND device_id IS NOT NULL`,
];

/** v4 配对码迁移：取消历史碰撞会话，并保证任一待确认码只对应一个会话。 */
const V4_STATEMENTS: readonly string[] = [
    `WITH ranked AS (
        SELECT id,
               row_number() OVER (
                   PARTITION BY display_code_hash
                   ORDER BY created_at DESC, id DESC
               ) AS rank
        FROM im_pairing_sessions
        WHERE status = 'pending'
    )
    UPDATE im_pairing_sessions AS session
    SET status = 'cancelled'
    FROM ranked
    WHERE session.id = ranked.id AND ranked.rank > 1`,
    `CREATE UNIQUE INDEX IF NOT EXISTS im_pairing_sessions_pending_display_code_hash_uq
        ON im_pairing_sessions (display_code_hash)
        WHERE status = 'pending'`,
];

/** v5 配对会话约束：单设备仅保留一个待确认会话，并阻止新的非法状态与时间组合。 */
const V5_STATEMENTS: readonly string[] = [
    `WITH ranked AS (
        SELECT id,
               row_number() OVER (
                   PARTITION BY device_id
                   ORDER BY created_at DESC, id DESC
               ) AS rank
        FROM im_pairing_sessions
        WHERE status = 'pending'
    )
    UPDATE im_pairing_sessions AS session
    SET status = 'cancelled'
    FROM ranked
    WHERE session.id = ranked.id AND ranked.rank > 1`,
    `CREATE UNIQUE INDEX IF NOT EXISTS im_pairing_sessions_pending_device_id_uq
        ON im_pairing_sessions (device_id)
        WHERE status = 'pending'`,
    'ALTER TABLE im_pairing_sessions DROP CONSTRAINT IF EXISTS im_pairing_sessions_status_check',
    `ALTER TABLE im_pairing_sessions
        ADD CONSTRAINT im_pairing_sessions_status_check
        CHECK (status IN ('pending', 'confirmed', 'expired', 'cancelled')) NOT VALID`,
    'ALTER TABLE im_pairing_sessions DROP CONSTRAINT IF EXISTS im_pairing_sessions_time_order_check',
    `ALTER TABLE im_pairing_sessions
        ADD CONSTRAINT im_pairing_sessions_time_order_check
        CHECK (expires_at > created_at) NOT VALID`,
    'ALTER TABLE im_pairing_sessions DROP CONSTRAINT IF EXISTS im_pairing_sessions_confirmation_check',
    `ALTER TABLE im_pairing_sessions
        ADD CONSTRAINT im_pairing_sessions_confirmation_check
        CHECK (
            (status = 'confirmed' AND confirmed_at IS NOT NULL AND confirmed_at >= created_at AND confirmed_at < expires_at)
            OR (status <> 'confirmed' AND confirmed_at IS NULL)
        ) NOT VALID`,
];

/** v6 注册设备：只保存固定长度 SHA-256 摘要。 */
const V6_STATEMENTS: readonly string[] = [
    `CREATE TABLE IF NOT EXISTS im_devices (
        device_id text PRIMARY KEY,
        user_id text NOT NULL,
        token_digest bytea NOT NULL UNIQUE,
        status text NOT NULL CHECK (status IN ('active', 'revoked')),
        created_at timestamptz NOT NULL,
        updated_at timestamptz NOT NULL,
        CHECK (octet_length(token_digest) = 32)
    )`,
    'CREATE INDEX IF NOT EXISTS im_devices_user_id_idx ON im_devices (user_id)',
];

/** v7 清理历史冲突，并实施设备与外部身份各自一对一的 active 约束。 */
const V7_STATEMENTS: readonly string[] = [
    `UPDATE im_bindings SET status = 'unbound', unbound_at = now()
     WHERE status = 'active' AND device_id IS NULL`,
    `WITH ranked AS (
        SELECT id,
               row_number() OVER (PARTITION BY device_id ORDER BY bound_at DESC, id DESC) AS device_rank,
               row_number() OVER (PARTITION BY external_identity_id ORDER BY bound_at DESC, id DESC) AS identity_rank
        FROM im_bindings
        WHERE status = 'active'
    )
    UPDATE im_bindings AS binding
    SET status = 'unbound', unbound_at = now()
    FROM ranked
    WHERE binding.id = ranked.id AND (ranked.device_rank > 1 OR ranked.identity_rank > 1)`,
    'DROP INDEX IF EXISTS im_bindings_active_user_device_identity_uq',
    `CREATE UNIQUE INDEX IF NOT EXISTS im_bindings_active_device_uq
        ON im_bindings (device_id) WHERE status = 'active'`,
    `CREATE UNIQUE INDEX IF NOT EXISTS im_bindings_active_external_identity_uq
        ON im_bindings (external_identity_id) WHERE status = 'active'`,
    'ALTER TABLE im_bindings DROP CONSTRAINT IF EXISTS im_bindings_active_device_check',
    `ALTER TABLE im_bindings ADD CONSTRAINT im_bindings_active_device_check
        CHECK (status <> 'active' OR device_id IS NOT NULL) NOT VALID`,
    'ALTER TABLE im_bindings VALIDATE CONSTRAINT im_bindings_active_device_check',
];

/** v8 设备语音动作事实：独立于 IM Action 保存，支持语音先执行与重启补报。 */
const V8_STATEMENTS: readonly string[] = [
    `CREATE TABLE IF NOT EXISTS im_reminder_action_facts (
        event_id text PRIMARY KEY,
        fingerprint text NOT NULL,
        schema_version text NOT NULL,
        correlation_id text NOT NULL,
        device_id text NOT NULL,
        reminder_trigger_id text NOT NULL,
        operation_id text NOT NULL,
        action text NOT NULL CHECK (action IN ('acknowledge', 'snooze')),
        status text NOT NULL CHECK (status IN ('succeeded', 'retryable_failed', 'failed', 'expired')),
        occurred_at timestamptz NOT NULL,
        next_trigger_at timestamptz,
        error_code text,
        details jsonb,
        received_at timestamptz NOT NULL
    )`,
    `CREATE INDEX IF NOT EXISTS im_reminder_action_facts_device_trigger_idx
        ON im_reminder_action_facts (device_id, reminder_trigger_id, occurred_at DESC, received_at DESC)`,
];

/** v9 为设备 operationId 建立唯一约束，防止不同 eventId 重复消费同一动作。 */
const V9_STATEMENTS: readonly string[] = [
    `CREATE UNIQUE INDEX IF NOT EXISTS im_reminder_action_facts_device_operation_uq
        ON im_reminder_action_facts (device_id, operation_id)`,
];

/** v10 保证同一设备提醒业务时间只有一个首次动作事实，收口数据库并发写入。 */
const V10_STATEMENTS: readonly string[] = [
    `WITH ranked AS (
        SELECT event_id,
               row_number() OVER (
                   PARTITION BY device_id, reminder_trigger_id, occurred_at
                   ORDER BY received_at ASC, event_id ASC
               ) AS rank
        FROM im_reminder_action_facts
    )
    DELETE FROM im_reminder_action_facts AS facts
    USING ranked
    WHERE facts.event_id = ranked.event_id AND ranked.rank > 1`,
    `CREATE UNIQUE INDEX IF NOT EXISTS im_reminder_action_facts_device_trigger_time_uq
        ON im_reminder_action_facts (device_id, reminder_trigger_id, occurred_at)`,
];

/** 按版本号索引的迁移脚本；下标 i 对应版本 i+1。 */
const VERSIONED_STATEMENTS: readonly (readonly string[])[] = [
    V1_STATEMENTS,
    V2_STATEMENTS,
    V3_STATEMENTS,
    V4_STATEMENTS,
    V5_STATEMENTS,
    V6_STATEMENTS,
    V7_STATEMENTS,
    V8_STATEMENTS,
    V9_STATEMENTS,
    V10_STATEMENTS,
];

/**
 * 以版本管理方式应用 IM Gateway 表结构与索引。
 *
 * 使用会话级咨询锁串行化跨实例迁移，并在单笔事务内应用全部 DDL 与版本行：
 * 中途失败会整体回滚，不会残留半套 schema；版本行仅在 DDL 全部成功后可见。
 * @param executor 专用连接客户端，保证锁在会话内持有、跨事务不释放。
 * @returns 迁移完成后兑现的 Promise。
 */
export async function applySchema(executor: SqlExecutor): Promise<void> {
    await executor.query('SELECT pg_advisory_lock($1)', [MIGRATION_LOCK_KEY]);
    try {
        // 版本表引导：先于业务 DDL 事务创建，用于读取当前版本。
        await executor.query(
            `CREATE TABLE IF NOT EXISTS ${SCHEMA_MIGRATIONS_TABLE} (
                version integer PRIMARY KEY,
                applied_at timestamptz NOT NULL DEFAULT now()
            )`,
        );
        const current = await queryOne(
            executor,
            `SELECT COALESCE(MAX(version), 0) AS version FROM ${SCHEMA_MIGRATIONS_TABLE}`,
            [],
        );
        const currentVersion = (current?.version as number | undefined) ?? 0;
        if (currentVersion >= SCHEMA_VERSION) return;
        await executor.query('BEGIN');
        try {
            for (let version = currentVersion + 1; version <= SCHEMA_VERSION; version++) {
                const statements = VERSIONED_STATEMENTS[version - 1];
                if (statements === undefined) {
                    throw new Error(`schema migration scripts missing for version ${version}`);
                }
                for (const statement of statements) {
                    await executor.query(statement);
                }
                await executor.query(`INSERT INTO ${SCHEMA_MIGRATIONS_TABLE} (version) VALUES ($1)`, [version]);
            }
            await executor.query('COMMIT');
        } catch (error) {
            await executor.query('ROLLBACK');
            throw error;
        }
    } finally {
        await executor.query('SELECT pg_advisory_unlock($1)', [MIGRATION_LOCK_KEY]);
    }
}
