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

const char kDeleteSchedule[] = "DELETE FROM schedule WHERE id = ?";

}  // namespace voicelife::storage_sqlite::sql
