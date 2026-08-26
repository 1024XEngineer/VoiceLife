#include "voicelife/storage_memory/memory_schedule_reminder_task_repository.h"

#include <algorithm>

namespace voicelife::storage_memory {
namespace {
using schedule::ScheduleReminderActionKind;
using schedule::ScheduleReminderBusinessStatus;
using schedule::ScheduleReminderTask;
using schedule::ScheduleReminderTimerStatus;

bool ValidAction(const ScheduleReminderTask& task) {
    if (!task.action_operation_id.has_value()) {
        return !task.action_kind.has_value() && !task.action_occurred_at.has_value() &&
               !task.action_next_trigger_at.has_value();
    }
    if (task.action_operation_id->empty() || !task.action_kind.has_value() || !task.action_occurred_at.has_value()) {
        return false;
    }
    if (*task.action_kind == ScheduleReminderActionKind::kAcknowledge) {
        return task.business_status == ScheduleReminderBusinessStatus::kAcknowledged &&
               !task.action_next_trigger_at.has_value();
    }
    return *task.action_kind == ScheduleReminderActionKind::kSnooze &&
           task.business_status == ScheduleReminderBusinessStatus::kSnoozed && task.action_next_trigger_at.has_value();
}

bool Valid(const ScheduleReminderTask& task) {
    const int business_status = static_cast<int>(task.business_status);
    const int timer_status = static_cast<int>(task.timer_status);
    return task.schedule_id > 0 && task.chain_id > 0 && task.attempt > 0 && task.attempt <= 3 &&
           task.trigger_at != schedule::DateTime{} &&
           business_status >= static_cast<int>(ScheduleReminderBusinessStatus::kScheduled) &&
           business_status <= static_cast<int>(ScheduleReminderBusinessStatus::kSnoozed) &&
           timer_status >= static_cast<int>(ScheduleReminderTimerStatus::kPending) &&
           timer_status <= static_cast<int>(ScheduleReminderTimerStatus::kFailed) && ValidAction(task);
}
}  // namespace

Result<schedule::ScheduleReminderTask> MemoryScheduleReminderTaskRepository::Insert(
    const schedule::ScheduleReminderTask& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Valid(task)) return Result<ScheduleReminderTask>::Failure(ErrorCode::kInvalidArgument, "提醒任务字段无效");
    if (std::any_of(tasks_.begin(), tasks_.end(), [&task](const auto& value) {
            return value.chain_id == task.chain_id && value.attempt == task.attempt;
        })) {
        return Result<ScheduleReminderTask>::Failure(ErrorCode::kAlreadyExists, "提醒链尝试次数已存在");
    }
    if (task.timing_task_id.has_value() && std::any_of(tasks_.begin(), tasks_.end(), [&task](const auto& value) {
            return value.timing_task_id == task.timing_task_id;
        })) {
        return Result<ScheduleReminderTask>::Failure(ErrorCode::kAlreadyExists, "Timing task 标识已存在");
    }
    if (task.action_operation_id.has_value() && std::any_of(tasks_.begin(), tasks_.end(), [&task](const auto& value) {
            return value.action_operation_id == task.action_operation_id;
        })) {
        return Result<ScheduleReminderTask>::Failure(ErrorCode::kAlreadyExists, "提醒动作 operationId 已存在");
    }
    ScheduleReminderTask stored = task;
    stored.id = next_id_++;
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    if (stored.created_at == schedule::DateTime{}) stored.created_at = now;
    if (stored.updated_at == schedule::DateTime{}) stored.updated_at = stored.created_at;
    tasks_.push_back(stored);
    return Result<ScheduleReminderTask>::Success(std::move(stored));
}

Status MemoryScheduleReminderTaskRepository::Update(const ScheduleReminderTask& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found =
        std::find_if(tasks_.begin(), tasks_.end(), [&task](const auto& value) { return value.id == task.id; });
    if (found == tasks_.end()) return Status::Error(ErrorCode::kNotFound, "提醒任务不存在");
    if (!Valid(task)) return Status::Error(ErrorCode::kInvalidArgument, "提醒任务字段无效");
    if (std::any_of(tasks_.begin(), tasks_.end(), [&task](const auto& value) {
            return value.id != task.id && value.chain_id == task.chain_id && value.attempt == task.attempt;
        }))
        return Status::Error(ErrorCode::kAlreadyExists, "提醒链尝试次数已存在");
    if (task.timing_task_id.has_value() && std::any_of(tasks_.begin(), tasks_.end(), [&task](const auto& value) {
            return value.id != task.id && value.timing_task_id == task.timing_task_id;
        }))
        return Status::Error(ErrorCode::kAlreadyExists, "Timing task 标识已存在");
    if (task.action_operation_id.has_value() && std::any_of(tasks_.begin(), tasks_.end(), [&task](const auto& value) {
            return value.id != task.id && value.action_operation_id == task.action_operation_id;
        }))
        return Status::Error(ErrorCode::kAlreadyExists, "提醒动作 operationId 已存在");
    *found = task;
    return Status::Ok();
}

Result<ScheduleReminderTask> MemoryScheduleReminderTaskRepository::FindById(int64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(tasks_.begin(), tasks_.end(), [id](const auto& value) { return value.id == id; });
    if (found == tasks_.end()) return Result<ScheduleReminderTask>::Failure(ErrorCode::kNotFound, "提醒任务不存在");
    return Result<ScheduleReminderTask>::Success(*found);
}

Result<ScheduleReminderTask> MemoryScheduleReminderTaskRepository::FindByTimingTaskId(
    std::string_view timing_task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(tasks_.begin(), tasks_.end(), [timing_task_id](const auto& value) {
        return value.timing_task_id.has_value() && *value.timing_task_id == timing_task_id;
    });
    if (found == tasks_.end()) return Result<ScheduleReminderTask>::Failure(ErrorCode::kNotFound, "提醒任务不存在");
    return Result<ScheduleReminderTask>::Success(*found);
}

Result<std::vector<ScheduleReminderTask>> MemoryScheduleReminderTaskRepository::FindBySchedule(
    schedule::ScheduleId schedule_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScheduleReminderTask> result;
    for (const auto& task : tasks_)
        if (task.schedule_id == schedule_id) result.push_back(task);
    return Result<std::vector<ScheduleReminderTask>>::Success(std::move(result));
}

Result<std::vector<ScheduleReminderTask>> MemoryScheduleReminderTaskRepository::FindAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Result<std::vector<ScheduleReminderTask>>::Success(tasks_);
}

Result<std::vector<ScheduleReminderTask>> MemoryScheduleReminderTaskRepository::FindTriggered(
    schedule::DateTime from, schedule::DateTime to) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScheduleReminderTask> result;
    for (const auto& task : tasks_) {
        if (task.triggered_at.has_value() && *task.triggered_at >= from && *task.triggered_at <= to &&
            task.timer_status == ScheduleReminderTimerStatus::kTriggered &&
            (task.business_status == ScheduleReminderBusinessStatus::kWaitingAcknowledgement ||
             task.business_status == ScheduleReminderBusinessStatus::kExhausted))
            result.push_back(task);
    }
    return Result<std::vector<ScheduleReminderTask>>::Success(std::move(result));
}

}  // namespace voicelife::storage_memory
