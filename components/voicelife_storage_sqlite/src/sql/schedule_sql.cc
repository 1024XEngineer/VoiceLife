#include "schedule_sql.h"

namespace voicelife::storage_sqlite::sql {

const char kCreateScheduleSchema[] = R"sql(
CREATE TABLE IF NOT EXISTS schedules (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event TEXT NOT NULL,
    start_time INTEGER,
    end_time INTEGER,
    location TEXT,
    notes TEXT,
    reminder_id INTEGER,
    status INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_schedules_start_time ON schedules(start_time, id);
)sql";

const char kInsertSchedule[] = R"sql(
INSERT INTO schedules (
    event, start_time, end_time, location, notes, reminder_id, status, created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
)sql";

const char kFindAllSchedules[] = R"sql(
SELECT id, event, start_time, end_time, location, notes, reminder_id, status, created_at, updated_at
FROM schedules
ORDER BY start_time IS NULL, start_time, id
)sql";

}  // namespace voicelife::storage_sqlite::sql
