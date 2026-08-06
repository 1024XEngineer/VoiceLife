import type { SqlExecutor } from './sql.js';

/** IM Gateway 持久化表清单，按外键依赖顺序排列，供清空与诊断使用。 */
export const IM_TABLES = [
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
    'im_outbox_events',
] as const;

/** 幂等迁移脚本：全部使用 IF NOT EXISTS，可重复执行。 */
const MIGRATION_STATEMENTS: readonly string[] = [
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

/**
 * 幂等应用 IM Gateway 表结构与索引。
 * @param executor 可执行参数化 SQL 的连接池或事务客户端。
 * @returns 迁移完成后兑现的 Promise。
 */
export async function applySchema(executor: SqlExecutor): Promise<void> {
    for (const statement of MIGRATION_STATEMENTS) {
        await executor.query(statement);
    }
}
