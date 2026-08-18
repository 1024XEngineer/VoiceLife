#include "schedule_exception_sql.h"

namespace voicelife::storage_sqlite::sql {

const char kUpsertScheduleException[] = R"sql(
INSERT INTO schedule_rule_exception (
    rule_id, original_start_time, schedule_id, type,
    override_start_time, override_end_time, override_event, override_location, override_notes,
    created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(rule_id, original_start_time) DO UPDATE SET
    schedule_id = excluded.schedule_id,
    type = excluded.type,
    override_start_time = excluded.override_start_time,
    override_end_time = excluded.override_end_time,
    override_event = excluded.override_event,
    override_location = excluded.override_location,
    override_notes = excluded.override_notes,
    updated_at = excluded.updated_at
)sql";

const char kFindExceptionsByRule[] = R"sql(
SELECT id, rule_id, original_start_time, schedule_id, type,
    override_start_time, override_end_time, override_event, override_location, override_notes,
    created_at, updated_at
FROM schedule_rule_exception WHERE rule_id = ?
ORDER BY original_start_time, id
)sql";

const char kFindExceptionByRuleAndTime[] = R"sql(
SELECT id, rule_id, original_start_time, schedule_id, type,
    override_start_time, override_end_time, override_event, override_location, override_notes,
    created_at, updated_at
FROM schedule_rule_exception WHERE rule_id = ? AND original_start_time = ?
)sql";

const char kDeleteFutureExceptionsByRule[] =
    "DELETE FROM schedule_rule_exception WHERE rule_id = ? AND original_start_time >= ?";

const char kDeleteExceptionsByRule[] = "DELETE FROM schedule_rule_exception WHERE rule_id = ?";

}  // namespace voicelife::storage_sqlite::sql
