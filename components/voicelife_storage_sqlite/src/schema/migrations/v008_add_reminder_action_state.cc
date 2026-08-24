#include "schema/migrations/v008_add_reminder_action_state.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

constexpr char kAddReminderActionState[] = R"sql(
ALTER TABLE schedule_reminder_task RENAME TO schedule_reminder_task_v007;
CREATE TABLE schedule_reminder_task (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    schedule_id INTEGER NOT NULL,
    event TEXT NOT NULL,
    chain_id INTEGER NOT NULL,
    attempt INTEGER NOT NULL CHECK (attempt BETWEEN 1 AND 3),
    timing_task_id TEXT,
    trigger_at INTEGER NOT NULL,
    business_status INTEGER NOT NULL CHECK (business_status BETWEEN 1 AND 6),
    timer_status INTEGER NOT NULL CHECK (timer_status BETWEEN 1 AND 5),
    triggered_at INTEGER,
    action_operation_id TEXT UNIQUE,
    action_kind INTEGER CHECK (action_kind IN (1, 2)),
    action_occurred_at INTEGER,
    action_next_trigger_at INTEGER,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (chain_id, attempt),
    UNIQUE (timing_task_id),
    CHECK (
        (action_operation_id IS NULL AND action_kind IS NULL AND action_occurred_at IS NULL AND
         action_next_trigger_at IS NULL) OR
        (action_operation_id IS NOT NULL AND length(action_operation_id) > 0 AND action_kind = 1 AND
         action_occurred_at IS NOT NULL AND action_next_trigger_at IS NULL AND business_status = 3) OR
        (action_operation_id IS NOT NULL AND length(action_operation_id) > 0 AND action_kind = 2 AND
         action_occurred_at IS NOT NULL AND action_next_trigger_at IS NOT NULL AND business_status = 6)
    )
);
INSERT INTO schedule_reminder_task (
    id, schedule_id, event, chain_id, attempt, timing_task_id, trigger_at, business_status,
    timer_status, triggered_at, created_at, updated_at
)
SELECT id, schedule_id, event, chain_id, attempt, timing_task_id, trigger_at, business_status,
       timer_status, triggered_at, created_at, updated_at
FROM schedule_reminder_task_v007;
DROP TABLE schedule_reminder_task_v007;
CREATE INDEX schedule_reminder_task_triggered_idx
    ON schedule_reminder_task (triggered_at, timer_status, business_status);
CREATE INDEX schedule_reminder_task_schedule_idx
    ON schedule_reminder_task (schedule_id, chain_id, attempt);
)sql";

}  // namespace

Status ApplyV008AddReminderActionState(SqliteDatabase& database) { return database.Execute(kAddReminderActionState); }

}  // namespace voicelife::storage_sqlite::schema::migrations
