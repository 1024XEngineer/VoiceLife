#include "schedule_rule_sql.h"

namespace voicelife::storage_sqlite::sql {

const char kInsertScheduleRule[] = R"sql(
INSERT INTO schedule_rule (
    event, location, notes, freq_type, interval_val, weekdays_mask, day_of_month,
    month_of_year, monthly_mode, start_time, end_time, start_date, end_date,
    occurrence_count, status, created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)sql";

const char kUpdateScheduleRule[] = R"sql(
UPDATE schedule_rule SET event = ?, location = ?, notes = ?, freq_type = ?, interval_val = ?,
    weekdays_mask = ?, day_of_month = ?, month_of_year = ?, monthly_mode = ?, start_time = ?, end_time = ?,
    start_date = ?, end_date = ?, occurrence_count = ?, status = ?, created_at = ?, updated_at = ?
WHERE id = ?
)sql";

const char kFindAllScheduleRules[] = R"sql(
SELECT id, event, location, notes, freq_type, interval_val, weekdays_mask, day_of_month,
    month_of_year, monthly_mode, start_time, end_time, start_date, end_date,
    occurrence_count, status, created_at, updated_at
FROM schedule_rule
ORDER BY id
)sql";

const char kFindScheduleRuleById[] = R"sql(
SELECT id, event, location, notes, freq_type, interval_val, weekdays_mask, day_of_month,
    month_of_year, monthly_mode, start_time, end_time, start_date, end_date,
    occurrence_count, status, created_at, updated_at
FROM schedule_rule WHERE id = ?
)sql";

const char kCancelScheduleRuleById[] = "UPDATE schedule_rule SET status = 2, updated_at = ? WHERE id = ?";

const char kCancelSchedulesByRule[] = "UPDATE schedule SET status = 2, updated_at = ? WHERE rule_id = ? AND status = 1";

const char kDeleteFutureSchedulesByRule[] = "DELETE FROM schedule WHERE rule_id = ? AND start_time >= ?";

}  // namespace voicelife::storage_sqlite::sql
