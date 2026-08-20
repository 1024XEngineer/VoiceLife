#include "mapping/schedule_reminder_task_row_mapper.h"

#include <chrono>

namespace voicelife::storage_sqlite::mapping {
namespace {
schedule::DateTime ReadTime(const SqliteStatement& statement, int column) {
    return schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(column)}};
}
}

Status BindScheduleReminderTask(SqliteStatement& statement, const schedule::ScheduleReminderTask& task) {
    int index = 1;
    Status status = statement.BindInt64(index++, task.schedule_id);
    if (!status.ok()) return status;
    if (!(status = statement.BindInt64(index++, task.chain_id)).ok()) return status;
    if (!(status = statement.BindInt(index++, task.attempt)).ok()) return status;
    if (!(status = task.timing_task_id.has_value() ? statement.BindText(index++, *task.timing_task_id)
                                                   : statement.BindNull(index++)).ok()) return status;
    if (!(status = statement.BindInt64(index++, task.trigger_at.time_since_epoch().count())).ok()) return status;
    if (!(status = statement.BindInt(index++, static_cast<int>(task.business_status))).ok()) return status;
    if (!(status = statement.BindInt(index++, static_cast<int>(task.timer_status))).ok()) return status;
    if (!(status = task.triggered_at.has_value() ? statement.BindInt64(index++, task.triggered_at->time_since_epoch().count())
                                                 : statement.BindNull(index++)).ok()) return status;
    if (!(status = statement.BindInt64(index++, task.created_at.time_since_epoch().count())).ok()) return status;
    return statement.BindInt64(index, task.updated_at.time_since_epoch().count());
}

Result<schedule::ScheduleReminderTask> ReadScheduleReminderTask(const SqliteStatement& statement) {
    schedule::ScheduleReminderTask task;
    task.id = statement.ColumnInt64(0);
    task.schedule_id = statement.ColumnInt64(1);
    task.chain_id = statement.ColumnInt64(2);
    task.attempt = statement.ColumnInt(3);
    if (!statement.IsNull(4)) task.timing_task_id = statement.ColumnText(4);
    task.trigger_at = ReadTime(statement, 5);
    task.business_status = static_cast<schedule::ScheduleReminderBusinessStatus>(statement.ColumnInt(6));
    task.timer_status = static_cast<schedule::ScheduleReminderTimerStatus>(statement.ColumnInt(7));
    if (!statement.IsNull(8)) task.triggered_at = ReadTime(statement, 8);
    task.created_at = ReadTime(statement, 9);
    task.updated_at = ReadTime(statement, 10);
    return Result<schedule::ScheduleReminderTask>::Success(std::move(task));
}
}  // namespace voicelife::storage_sqlite::mapping
