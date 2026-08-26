#include "voicelife/storage_sqlite/sqlite_schedule_reminder_task_repository.h"

#include <chrono>
#include <utility>

#include "mapping/schedule_reminder_task_row_mapper.h"
#include "sql/schedule_reminder_task_sql.h"

namespace voicelife::storage_sqlite {
namespace {
using schedule::ScheduleReminderTask;
bool ValidAction(const ScheduleReminderTask& task) {
    if (!task.action_operation_id.has_value()) {
        return !task.action_kind.has_value() && !task.action_occurred_at.has_value() &&
               !task.action_next_trigger_at.has_value();
    }
    if (task.action_operation_id->empty() || !task.action_kind.has_value() || !task.action_occurred_at.has_value()) {
        return false;
    }
    if (*task.action_kind == schedule::ScheduleReminderActionKind::kAcknowledge) {
        return task.business_status == schedule::ScheduleReminderBusinessStatus::kAcknowledged &&
               !task.action_next_trigger_at.has_value();
    }
    return *task.action_kind == schedule::ScheduleReminderActionKind::kSnooze &&
           task.business_status == schedule::ScheduleReminderBusinessStatus::kSnoozed &&
           task.action_next_trigger_at.has_value();
}
bool Valid(const ScheduleReminderTask& task) {
    const int business_status = static_cast<int>(task.business_status);
    const int timer_status = static_cast<int>(task.timer_status);
    return task.schedule_id > 0 && task.chain_id > 0 && task.attempt >= 1 && task.attempt <= 3 &&
           task.trigger_at != schedule::DateTime{} &&
           business_status >= static_cast<int>(schedule::ScheduleReminderBusinessStatus::kScheduled) &&
           business_status <= static_cast<int>(schedule::ScheduleReminderBusinessStatus::kSnoozed) &&
           timer_status >= static_cast<int>(schedule::ScheduleReminderTimerStatus::kPending) &&
           timer_status <= static_cast<int>(schedule::ScheduleReminderTimerStatus::kFailed) && ValidAction(task);
}
Status Invalid() { return Status::Error(ErrorCode::kInvalidArgument, "提醒任务字段无效"); }
Status Unavailable() { return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库尚未打开"); }
Result<ScheduleReminderTask> ReadOne(SqliteStatement& statement) {
    const auto step = statement.Step();
    if (!step.ok()) return Result<ScheduleReminderTask>::Failure(step.status.code, step.status.message);
    if (*step.value != SqliteStep::kRow)
        return Result<ScheduleReminderTask>::Failure(ErrorCode::kNotFound, "提醒任务不存在");
    return mapping::ReadScheduleReminderTask(statement);
}
Result<std::vector<ScheduleReminderTask>> ReadMany(SqliteStatement& statement) {
    std::vector<ScheduleReminderTask> result;
    while (true) {
        const auto step = statement.Step();
        if (!step.ok())
            return Result<std::vector<ScheduleReminderTask>>::Failure(step.status.code, step.status.message);
        if (*step.value == SqliteStep::kDone) break;
        const auto row = mapping::ReadScheduleReminderTask(statement);
        if (!row.ok()) return Result<std::vector<ScheduleReminderTask>>::Failure(row.status.code, row.status.message);
        result.push_back(*row.value);
    }
    return Result<std::vector<ScheduleReminderTask>>::Success(std::move(result));
}
}  // namespace

SqliteScheduleReminderTaskRepository::SqliteScheduleReminderTaskRepository(SqliteDatabase& database)
    : database_(database) {}

Result<ScheduleReminderTask> SqliteScheduleReminderTaskRepository::Insert(const ScheduleReminderTask& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen())
        return Result<ScheduleReminderTask>::Failure(ErrorCode::kUnavailable, Unavailable().message);
    if (!Valid(task)) return Result<ScheduleReminderTask>::Failure(ErrorCode::kInvalidArgument, Invalid().message);
    ScheduleReminderTask stored = task;
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    if (stored.created_at == schedule::DateTime{}) stored.created_at = now;
    if (stored.updated_at == schedule::DateTime{}) stored.updated_at = stored.created_at;
    auto prepared = database_.Prepare(sql::kInsertScheduleReminderTask);
    if (!prepared.ok()) return Result<ScheduleReminderTask>::Failure(prepared.status.code, prepared.status.message);
    auto statement = std::move(*prepared.value);
    auto status = mapping::BindScheduleReminderTask(statement, stored);
    if (!status.ok()) return Result<ScheduleReminderTask>::Failure(status.code, status.message);
    auto step = statement.Step();
    if (!step.ok()) return Result<ScheduleReminderTask>::Failure(step.status.code, step.status.message);
    stored.id = statement.LastInsertRowId();
    return Result<ScheduleReminderTask>::Success(std::move(stored));
}

Status SqliteScheduleReminderTaskRepository::Update(const ScheduleReminderTask& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return Unavailable();
    if (!Valid(task)) return Invalid();
    auto prepared = database_.Prepare(sql::kUpdateScheduleReminderTask);
    if (!prepared.ok()) return prepared.status;
    auto statement = std::move(*prepared.value);
    auto status = mapping::BindScheduleReminderTask(statement, task);
    if (!status.ok()) return status;
    if (!(status = statement.BindInt64(16, task.id)).ok()) return status;
    auto step = statement.Step();
    if (!step.ok()) return step.status;
    return statement.Changes() == 1 ? Status::Ok() : Status::Error(ErrorCode::kNotFound, "提醒任务不存在");
}

Result<ScheduleReminderTask> SqliteScheduleReminderTaskRepository::FindById(int64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen())
        return Result<ScheduleReminderTask>::Failure(ErrorCode::kUnavailable, Unavailable().message);
    auto prepared = database_.Prepare(sql::kFindScheduleReminderTaskById);
    if (!prepared.ok()) return Result<ScheduleReminderTask>::Failure(prepared.status.code, prepared.status.message);
    auto statement = std::move(*prepared.value);
    auto status = statement.BindInt64(1, id);
    if (!status.ok()) return Result<ScheduleReminderTask>::Failure(status.code, status.message);
    return ReadOne(statement);
}

Result<ScheduleReminderTask> SqliteScheduleReminderTaskRepository::FindByTimingTaskId(
    std::string_view timing_task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen())
        return Result<ScheduleReminderTask>::Failure(ErrorCode::kUnavailable, Unavailable().message);
    auto prepared = database_.Prepare(sql::kFindScheduleReminderTaskByTimingTaskId);
    if (!prepared.ok()) return Result<ScheduleReminderTask>::Failure(prepared.status.code, prepared.status.message);
    auto statement = std::move(*prepared.value);
    auto status = statement.BindText(1, timing_task_id);
    if (!status.ok()) return Result<ScheduleReminderTask>::Failure(status.code, status.message);
    return ReadOne(statement);
}

Result<std::vector<ScheduleReminderTask>> SqliteScheduleReminderTaskRepository::FindBySchedule(
    schedule::ScheduleId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen())
        return Result<std::vector<ScheduleReminderTask>>::Failure(ErrorCode::kUnavailable, Unavailable().message);
    auto prepared = database_.Prepare(sql::kFindScheduleReminderTasksBySchedule);
    if (!prepared.ok())
        return Result<std::vector<ScheduleReminderTask>>::Failure(prepared.status.code, prepared.status.message);
    auto statement = std::move(*prepared.value);
    auto status = statement.BindInt64(1, id);
    if (!status.ok()) return Result<std::vector<ScheduleReminderTask>>::Failure(status.code, status.message);
    return ReadMany(statement);
}

Result<std::vector<ScheduleReminderTask>> SqliteScheduleReminderTaskRepository::FindAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen())
        return Result<std::vector<ScheduleReminderTask>>::Failure(ErrorCode::kUnavailable, Unavailable().message);
    auto prepared = database_.Prepare(sql::kFindAllScheduleReminderTasks);
    if (!prepared.ok())
        return Result<std::vector<ScheduleReminderTask>>::Failure(prepared.status.code, prepared.status.message);
    auto statement = std::move(*prepared.value);
    return ReadMany(statement);
}

Result<std::vector<ScheduleReminderTask>> SqliteScheduleReminderTaskRepository::FindTriggered(
    schedule::DateTime from, schedule::DateTime to) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen())
        return Result<std::vector<ScheduleReminderTask>>::Failure(ErrorCode::kUnavailable, Unavailable().message);
    auto prepared = database_.Prepare(sql::kFindTriggeredScheduleReminderTasks);
    if (!prepared.ok())
        return Result<std::vector<ScheduleReminderTask>>::Failure(prepared.status.code, prepared.status.message);
    auto statement = std::move(*prepared.value);
    auto status = statement.BindInt64(1, from.time_since_epoch().count());
    if (!status.ok()) return Result<std::vector<ScheduleReminderTask>>::Failure(status.code, status.message);
    status = statement.BindInt64(2, to.time_since_epoch().count());
    if (!status.ok()) return Result<std::vector<ScheduleReminderTask>>::Failure(status.code, status.message);
    return ReadMany(statement);
}
}  // namespace voicelife::storage_sqlite
