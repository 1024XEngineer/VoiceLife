#include "mapping/schedule_reminder_task_row_mapper.h"

#include <chrono>

namespace voicelife::storage_sqlite::mapping {
namespace {
schedule::DateTime ReadTime(const SqliteStatement& statement, int column) {
    return schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(column)}};
}

bool ValidBusinessStatus(int value) {
    return value >= static_cast<int>(schedule::ScheduleReminderBusinessStatus::kScheduled) &&
           value <= static_cast<int>(schedule::ScheduleReminderBusinessStatus::kSnoozed);
}

bool ValidTimerStatus(int value) {
    return value >= static_cast<int>(schedule::ScheduleReminderTimerStatus::kPending) &&
           value <= static_cast<int>(schedule::ScheduleReminderTimerStatus::kFailed);
}
}  // namespace

Status BindScheduleReminderTask(SqliteStatement& statement, const schedule::ScheduleReminderTask& task) {
    int index = 1;
    Status status = statement.BindInt64(index++, task.schedule_id);
    if (!status.ok()) return status;
    if (!(status = statement.BindText(index++, task.event)).ok()) return status;
    if (!(status = statement.BindInt64(index++, task.chain_id)).ok()) return status;
    if (!(status = statement.BindInt(index++, task.attempt)).ok()) return status;
    if (!(status = task.timing_task_id.has_value() ? statement.BindText(index++, *task.timing_task_id)
                                                   : statement.BindNull(index++))
             .ok())
        return status;
    if (!(status = statement.BindInt64(index++, task.trigger_at.time_since_epoch().count())).ok()) return status;
    if (!(status = statement.BindInt(index++, static_cast<int>(task.business_status))).ok()) return status;
    if (!(status = statement.BindInt(index++, static_cast<int>(task.timer_status))).ok()) return status;
    if (!(status = task.triggered_at.has_value()
                       ? statement.BindInt64(index++, task.triggered_at->time_since_epoch().count())
                       : statement.BindNull(index++))
             .ok())
        return status;
    if (!(status = task.action_operation_id.has_value() ? statement.BindText(index++, *task.action_operation_id)
                                                        : statement.BindNull(index++))
             .ok())
        return status;
    if (!(status = task.action_kind.has_value() ? statement.BindInt(index++, static_cast<int>(*task.action_kind))
                                                : statement.BindNull(index++))
             .ok())
        return status;
    if (!(status = task.action_occurred_at.has_value()
                       ? statement.BindInt64(index++, task.action_occurred_at->time_since_epoch().count())
                       : statement.BindNull(index++))
             .ok())
        return status;
    if (!(status = task.action_next_trigger_at.has_value()
                       ? statement.BindInt64(index++, task.action_next_trigger_at->time_since_epoch().count())
                       : statement.BindNull(index++))
             .ok())
        return status;
    if (!(status = statement.BindInt64(index++, task.created_at.time_since_epoch().count())).ok()) return status;
    return statement.BindInt64(index, task.updated_at.time_since_epoch().count());
}

Result<schedule::ScheduleReminderTask> ReadScheduleReminderTask(const SqliteStatement& statement) {
    const int business_status = statement.ColumnInt(7);
    const int timer_status = statement.ColumnInt(8);
    if (!ValidBusinessStatus(business_status) || !ValidTimerStatus(timer_status)) {
        return Result<schedule::ScheduleReminderTask>::Failure(ErrorCode::kInternal, "提醒任务状态值非法");
    }
    schedule::ScheduleReminderTask task;
    task.id = statement.ColumnInt64(0);
    task.schedule_id = statement.ColumnInt64(1);
    task.event = statement.ColumnText(2);
    task.chain_id = statement.ColumnInt64(3);
    task.attempt = statement.ColumnInt(4);
    if (!statement.IsNull(5)) task.timing_task_id = statement.ColumnText(5);
    task.trigger_at = ReadTime(statement, 6);
    task.business_status = static_cast<schedule::ScheduleReminderBusinessStatus>(business_status);
    task.timer_status = static_cast<schedule::ScheduleReminderTimerStatus>(timer_status);
    if (!statement.IsNull(9)) task.triggered_at = ReadTime(statement, 9);
    if (!statement.IsNull(10)) task.action_operation_id = statement.ColumnText(10);
    if (!statement.IsNull(11)) {
        const int action_kind = statement.ColumnInt(11);
        if (action_kind < static_cast<int>(schedule::ScheduleReminderActionKind::kAcknowledge) ||
            action_kind > static_cast<int>(schedule::ScheduleReminderActionKind::kSnooze)) {
            return Result<schedule::ScheduleReminderTask>::Failure(ErrorCode::kInternal, "提醒动作类型值非法");
        }
        task.action_kind = static_cast<schedule::ScheduleReminderActionKind>(action_kind);
    }
    if (!statement.IsNull(12)) task.action_occurred_at = ReadTime(statement, 12);
    if (!statement.IsNull(13)) task.action_next_trigger_at = ReadTime(statement, 13);
    task.created_at = ReadTime(statement, 14);
    task.updated_at = ReadTime(statement, 15);
    return Result<schedule::ScheduleReminderTask>::Success(std::move(task));
}
}  // namespace voicelife::storage_sqlite::mapping
