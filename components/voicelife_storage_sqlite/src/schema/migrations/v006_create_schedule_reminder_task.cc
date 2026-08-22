#include "schema/migrations/v006_create_schedule_reminder_task.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

constexpr char kCreateScheduleReminderTask[] = R"sql(
CREATE TABLE schedule_reminder_task (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    schedule_id INTEGER NOT NULL,
    chain_id INTEGER NOT NULL,
    attempt INTEGER NOT NULL CHECK (attempt BETWEEN 1 AND 3),
    timing_task_id TEXT,
    trigger_at INTEGER NOT NULL,
    business_status INTEGER NOT NULL CHECK (business_status BETWEEN 1 AND 5),
    timer_status INTEGER NOT NULL CHECK (timer_status BETWEEN 1 AND 5),
    triggered_at INTEGER,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (chain_id, attempt),
    UNIQUE (timing_task_id)
);
CREATE INDEX schedule_reminder_task_triggered_idx
    ON schedule_reminder_task (triggered_at, timer_status, business_status);
CREATE INDEX schedule_reminder_task_schedule_idx
    ON schedule_reminder_task (schedule_id, chain_id, attempt);

INSERT INTO schedule_reminder_task (
    schedule_id, chain_id, attempt, timing_task_id, trigger_at,
    business_status, timer_status, triggered_at, created_at, updated_at
)
SELECT id, reminder_task_id, 1, CAST(reminder_task_id AS TEXT),
       COALESCE(start_time, created_at), 1, 1, NULL, created_at, updated_at
FROM schedule
WHERE reminder_task_id IS NOT NULL;

ALTER TABLE schedule RENAME TO schedule_v005;
CREATE TABLE schedule (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    rule_id INTEGER,
    event TEXT NOT NULL CHECK (length(event) <= 100),
    start_time INTEGER,
    end_time INTEGER,
    location TEXT CHECK (location IS NULL OR length(location) <= 100),
    notes TEXT CHECK (notes IS NULL OR length(notes) <= 200),
    status INTEGER NOT NULL DEFAULT 1 CHECK (status IN (1, 2, 3)),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    CHECK (end_time IS NULL OR (start_time IS NOT NULL AND end_time > start_time))
);
INSERT INTO schedule (
    id, rule_id, event, start_time, end_time, location, notes, status, created_at, updated_at
)
SELECT id, rule_id, event, start_time, end_time, location, notes, status, created_at, updated_at
FROM schedule_v005;
DROP TABLE schedule_v005;
CREATE INDEX schedule_rule_idx ON schedule (rule_id);
CREATE INDEX schedule_start_time_idx ON schedule (start_time);
)sql";
}

Status ApplyV006CreateScheduleReminderTask(SqliteDatabase& database) {
    return database.Execute(kCreateScheduleReminderTask);
}

}  // namespace voicelife::storage_sqlite::schema::migrations
