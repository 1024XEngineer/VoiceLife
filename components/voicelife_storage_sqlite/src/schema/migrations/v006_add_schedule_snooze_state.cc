#include "schema/migrations/v006_add_schedule_snooze_state.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

/** @brief 为日程实例增加已接受的推迟次数。 */
constexpr char kAddScheduleSnoozeCount[] = R"sql(
ALTER TABLE schedule ADD COLUMN snooze_count INTEGER NOT NULL DEFAULT 0;
)sql";

/** @brief 为日程实例增加可空的推迟重复提醒任务标识。 */
constexpr char kAddScheduleRepeatTaskId[] = R"sql(
ALTER TABLE schedule ADD COLUMN repeat_task_id INTEGER;
)sql";

/** @brief 为日程实例增加可空的推迟重复提醒计划触发时间（epoch 秒）。 */
constexpr char kAddScheduleRepeatTriggerAt[] = R"sql(
ALTER TABLE schedule ADD COLUMN repeat_trigger_at INTEGER;
)sql";

}  // namespace

Status ApplyV006AddScheduleSnoozeState(SqliteDatabase& database) {
    const Status count = database.Execute(kAddScheduleSnoozeCount);
    if (!count.ok()) return count;
    const Status task_id = database.Execute(kAddScheduleRepeatTaskId);
    if (!task_id.ok()) return task_id;
    return database.Execute(kAddScheduleRepeatTriggerAt);
}

}  // namespace voicelife::storage_sqlite::schema::migrations
