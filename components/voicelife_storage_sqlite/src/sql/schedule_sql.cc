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

const char kUpdateSchedule[] = R"sql(
UPDATE schedule SET event = ?, start_time = ?, end_time = ?, location = ?, notes = ?,
rule_id = ?, status = ?, created_at = ?, updated_at = ? WHERE id = ?
)sql";

const char kCancelSchedule[] = "UPDATE schedule SET status = 2, updated_at = ? WHERE id = ? AND status <> 2";

const char kFindScheduleById[] = R"sql(
SELECT id, event, start_time, end_time, location, notes, rule_id, status, created_at, updated_at
FROM schedule WHERE id = ?
)sql";

const char kDeleteSchedulePhysical[] = "DELETE FROM schedule WHERE id = ?";

const char kRestoreScheduleInsert[] = R"sql(
INSERT INTO schedule (id, event, start_time, end_time, location, notes, rule_id, status, created_at, updated_at)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)sql";

const char kRestoreScheduleUpdate[] = R"sql(
UPDATE schedule SET event = ?, start_time = ?, end_time = ?, location = ?, notes = ?,
rule_id = ?, status = ?, created_at = ?, updated_at = ? WHERE id = ?
)sql";

}  // namespace voicelife::storage_sqlite::sql
