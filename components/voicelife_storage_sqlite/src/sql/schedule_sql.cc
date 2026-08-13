#include "schedule_sql.h"

namespace voicelife::storage_sqlite::sql {

const char kInsertSchedule[] = R"sql(
INSERT INTO schedule (
    event, start_time, end_time, location, notes, rule_id, status, created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
)sql";

const char kFindAllSchedules[] = R"sql(
SELECT id, event, start_time, end_time, location, notes, rule_id, status, created_at, updated_at
FROM schedule
ORDER BY start_time IS NULL, start_time, id
)sql";

const char kFindDueSchedules[] = R"sql(
SELECT id, event, start_time, end_time, location, notes, rule_id, status, created_at, updated_at
FROM schedule
WHERE status = 1 AND start_time IS NOT NULL AND start_time <= ?
  AND NOT EXISTS (SELECT 1 FROM schedule_reminder_delivery WHERE schedule_id = schedule.id)
ORDER BY start_time, id
LIMIT ?
)sql";

const char kInsertReminderDelivery[] = R"sql(
INSERT OR IGNORE INTO schedule_reminder_delivery (schedule_id, delivered_at) VALUES (?, ?)
)sql";

const char kFindScheduleByIdempotencyKey[] = R"sql(
SELECT schedule.id, schedule.event, schedule.start_time, schedule.end_time, schedule.location, schedule.notes,
       schedule.rule_id, schedule.status, schedule.created_at, schedule.updated_at
FROM schedule
JOIN schedule_creation_request ON schedule_creation_request.schedule_id = schedule.id
WHERE schedule_creation_request.request_key = ?
)sql";

const char kInsertScheduleIdempotencyKey[] = R"sql(
INSERT INTO schedule_creation_request (request_key, schedule_id, created_at) VALUES (?, ?, ?)
)sql";

const char kUpdateSchedule[] = R"sql(
UPDATE schedule SET event = ?, start_time = ?, end_time = ?, location = ?, notes = ?,
rule_id = ?, status = ?, created_at = ?, updated_at = ? WHERE id = ?
)sql";

const char kDeleteSchedule[] = "DELETE FROM schedule WHERE id = ?";

}  // namespace voicelife::storage_sqlite::sql
