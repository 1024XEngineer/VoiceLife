#include "sql/operation_sql.h"

namespace voicelife::storage_sqlite::sql {

const char kInsertOperation[] = R"sql(
INSERT INTO operation_record (
    type, schedule_id, schedule_event, operated_at, active,
    previous_id, previous_event, previous_start_time, previous_end_time,
    previous_location, previous_notes, previous_rule_id, previous_status,
    previous_created_at, previous_updated_at
) VALUES (?, ?, ?, ?, 1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)sql";

const char kFindRecentOperations[] = R"sql(
SELECT id, type, schedule_id, schedule_event, operated_at, active,
       previous_id, previous_event, previous_start_time, previous_end_time,
       previous_location, previous_notes, previous_rule_id, previous_status,
       previous_created_at, previous_updated_at
FROM operation_record
WHERE active = 1 AND operated_at BETWEEN ? AND ?
ORDER BY operated_at DESC, id DESC
)sql";

const char kFindOperationById[] = R"sql(
SELECT id, type, schedule_id, schedule_event, operated_at, active,
       previous_id, previous_event, previous_start_time, previous_end_time,
       previous_location, previous_notes, previous_rule_id, previous_status,
       previous_created_at, previous_updated_at
FROM operation_record WHERE id = ?
)sql";

const char kDeactivateOperation[] = "UPDATE operation_record SET active = 0 WHERE id = ? AND active = 1";

}  // namespace voicelife::storage_sqlite::sql
