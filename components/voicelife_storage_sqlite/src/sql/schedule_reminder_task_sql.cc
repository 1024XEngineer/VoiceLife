#include "sql/schedule_reminder_task_sql.h"

namespace voicelife::storage_sqlite::sql {
const char kInsertScheduleReminderTask[] = R"sql(
INSERT INTO schedule_reminder_task
(schedule_id, event, chain_id, attempt, timing_task_id, trigger_at, business_status, timer_status,
 triggered_at, action_operation_id, action_kind, action_occurred_at, action_next_trigger_at, created_at, updated_at)
 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)sql";
const char kFindAllScheduleReminderTasks[] = R"sql(
SELECT id, schedule_id, event, chain_id, attempt, timing_task_id, trigger_at, business_status, timer_status,
       triggered_at, action_operation_id, action_kind, action_occurred_at, action_next_trigger_at, created_at, updated_at
FROM schedule_reminder_task ORDER BY trigger_at, id
)sql";
const char kFindScheduleReminderTaskById[] = R"sql(
SELECT id, schedule_id, event, chain_id, attempt, timing_task_id, trigger_at, business_status, timer_status,
       triggered_at, action_operation_id, action_kind, action_occurred_at, action_next_trigger_at, created_at, updated_at
FROM schedule_reminder_task WHERE id = ?
)sql";
const char kFindScheduleReminderTaskByTimingTaskId[] = R"sql(
SELECT id, schedule_id, event, chain_id, attempt, timing_task_id, trigger_at, business_status, timer_status,
       triggered_at, action_operation_id, action_kind, action_occurred_at, action_next_trigger_at, created_at, updated_at
FROM schedule_reminder_task WHERE timing_task_id = ?
)sql";
const char kFindScheduleReminderTasksBySchedule[] = R"sql(
SELECT id, schedule_id, event, chain_id, attempt, timing_task_id, trigger_at, business_status, timer_status,
       triggered_at, action_operation_id, action_kind, action_occurred_at, action_next_trigger_at, created_at, updated_at
FROM schedule_reminder_task WHERE schedule_id = ? ORDER BY chain_id, attempt, id
)sql";
const char kFindTriggeredScheduleReminderTasks[] = R"sql(
SELECT id, schedule_id, event, chain_id, attempt, timing_task_id, trigger_at, business_status, timer_status,
       triggered_at, action_operation_id, action_kind, action_occurred_at, action_next_trigger_at, created_at, updated_at
FROM schedule_reminder_task
WHERE triggered_at >= ? AND triggered_at <= ? AND timer_status = 2
  AND business_status IN (2, 4)
ORDER BY triggered_at, id
)sql";
const char kUpdateScheduleReminderTask[] = R"sql(
UPDATE schedule_reminder_task SET schedule_id = ?, event = ?, chain_id = ?, attempt = ?, timing_task_id = ?, trigger_at = ?,
 business_status = ?, timer_status = ?, triggered_at = ?, action_operation_id = ?, action_kind = ?,
 action_occurred_at = ?, action_next_trigger_at = ?, created_at = ?, updated_at = ? WHERE id = ?
)sql";
}  // namespace voicelife::storage_sqlite::sql
