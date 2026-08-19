#include "schedule_sql.h"

#include <string>

namespace voicelife::storage_sqlite::sql {

const char kInsertSchedule[] = R"sql(
INSERT INTO schedule (
    event, start_time, end_time, location, notes, rule_id, reminder_task_id,
    status, created_at, updated_at, snooze_count, repeat_task_id, repeat_trigger_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)sql";

const char kFindAllSchedules[] = R"sql(
SELECT id, event, start_time, end_time, location, notes, rule_id,
       reminder_task_id, status, created_at, updated_at, snooze_count,
       repeat_task_id, repeat_trigger_at
FROM schedule
ORDER BY start_time IS NULL, start_time, id
)sql";

const char kUpdateSchedule[] = R"sql(
UPDATE schedule SET event = ?, start_time = ?, end_time = ?, location = ?, notes = ?,
rule_id = ?, reminder_task_id = ?, status = ?, created_at = ?, updated_at = ?,
snooze_count = ?, repeat_task_id = ?, repeat_trigger_at = ? WHERE id = ?
)sql";

const char kCancelSchedule[] = "UPDATE schedule SET status = 2, updated_at = ? WHERE id = ? AND status <> 2";

const char kFindScheduleById[] = R"sql(
SELECT id, event, start_time, end_time, location, notes, rule_id,
       reminder_task_id, status, created_at, updated_at, snooze_count,
       repeat_task_id, repeat_trigger_at
FROM schedule WHERE id = ?
)sql";

const char kFindOverlappingSchedules[] = R"sql(
SELECT id, event, start_time, end_time, location, notes, rule_id,
       reminder_task_id, status, created_at, updated_at, snooze_count,
       repeat_task_id, repeat_trigger_at
FROM schedule
WHERE status = 1
  AND start_time IS NOT NULL
  AND start_time <= ?
  AND (end_time IS NULL OR end_time >= ?)
  AND (? IS NULL OR id <> ?)
ORDER BY start_time, id
)sql";

std::string BuildScheduleWhere() {
    return R"sql(
WHERE (?1 IS NULL OR id = ?1)
  AND (?2 IS NULL OR rule_id = ?2)
  AND (?3 IS NULL OR status = ?3)
  AND (?4 IS NULL OR event LIKE '%' || ?4 || '%' OR location LIKE '%' || ?4 || '%' OR notes LIKE '%' || ?4 || '%')
  AND (?5 IS NULL OR start_time >= ?5)
  AND (?6 IS NULL OR start_time <= ?6)
)sql";
}

std::string BuildScheduleFindSql(const schedule::QueryScheduleCommand& query) {
    (void)query;
    return R"sql(
SELECT id, event, start_time, end_time, location, notes, rule_id,
       reminder_task_id, status, created_at, updated_at, snooze_count,
       repeat_task_id, repeat_trigger_at
FROM schedule
)sql" + BuildScheduleWhere() +
           R"sql(
ORDER BY
  CASE
    WHEN ?4 IS NOT NULL AND lower(event) = lower(?4) THEN 100
    WHEN ?4 IS NOT NULL AND lower(event) LIKE lower(?4) || '%' THEN 80
    WHEN ?4 IS NOT NULL AND lower(event) LIKE '%' || lower(?4) || '%' THEN 60
    ELSE 0
  END DESC,
  start_time IS NULL,
  start_time,
  id
LIMIT ?7 OFFSET ?8
)sql";
}

std::string BuildScheduleCountSql(const schedule::QueryScheduleCommand& query) {
    (void)query;
    return R"sql(
SELECT COUNT(*)
FROM schedule
)sql" + BuildScheduleWhere();
}

}  // namespace voicelife::storage_sqlite::sql
