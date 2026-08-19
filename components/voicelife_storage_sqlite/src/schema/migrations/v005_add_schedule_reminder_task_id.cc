#include "schema/migrations/v005_add_schedule_reminder_task_id.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

/** @brief 为日程实例增加可空的一次性提醒任务标识。 */
constexpr char kAddScheduleReminderTaskId[] = R"sql(
ALTER TABLE schedule ADD COLUMN reminder_task_id INTEGER;
)sql";

}  // namespace

Status ApplyV005AddScheduleReminderTaskId(SqliteDatabase& database) {
    return database.Execute(kAddScheduleReminderTaskId);
}

}  // namespace voicelife::storage_sqlite::schema::migrations
