#include "voicelife/timing/timing_task.h"

#include <algorithm>
#include <utility>

namespace voicelife::timing {

namespace {

thread_local InMemoryTimingTaskRunner* active_callback_runner = nullptr;

}  // namespace

std::optional<TaskId> TaskId::Create(std::string value) {
    if (value.empty()) {
        return std::nullopt;
    }
    return TaskId(std::move(value));
}

TaskId::TaskId(std::string value) : value_(std::move(value)) {}

const std::string& TaskId::Value() const { return value_; }

bool CanTransition(TaskStatus from, TaskStatus to) {
    if (from == TaskStatus::kPending) {
        return to == TaskStatus::kExecuting || to == TaskStatus::kCancelled;
    }
    return from == TaskStatus::kExecuting && to == TaskStatus::kCompleted;
}

CommandAcceptance InMemoryTimingTaskRunner::RegisterTask(RegisterTaskCommand command) {
    if (active_callback_runner == this) {
        ApplyRegisterTask(std::move(command), *callback_applied_at_);
        return CommandAcceptance::kAccepted;
    }
    commands_.push_back(std::move(command));
    return CommandAcceptance::kAccepted;
}

CommandAcceptance InMemoryTimingTaskRunner::CancelTask(CancelTaskCommand command) {
    if (active_callback_runner == this) {
        ApplyCancelTask(std::move(command), *callback_applied_at_);
        return CommandAcceptance::kAccepted;
    }
    commands_.push_back(std::move(command));
    return CommandAcceptance::kAccepted;
}

size_t InMemoryTimingTaskRunner::ProcessPendingCommands(TriggerAt applied_at) {
    if (processing_commands_) {
        return 0;
    }
    processing_commands_ = true;
    struct ProcessingGuard {
        bool& processing;
        ~ProcessingGuard() { processing = false; }
    } processing_guard{processing_commands_};

    const auto processed_count = commands_.size();
    for (size_t index = 0; index < processed_count; ++index) {
        auto pending_command = std::move(commands_.front());
        commands_.pop_front();
        if (auto* cancel = std::get_if<CancelTaskCommand>(&pending_command)) {
            ApplyCancelTask(std::move(*cancel), applied_at);
            continue;
        }

        ApplyRegisterTask(std::move(std::get<RegisterTaskCommand>(pending_command)), applied_at);
    }
    return processed_count;
}

void InMemoryTimingTaskRunner::ApplyRegisterTask(RegisterTaskCommand command, TriggerAt applied_at) {
    if (std::find(used_task_ids_.begin(), used_task_ids_.end(), command.task_id.Value()) != used_task_ids_.end()) {
        if (command.on_result) {
            command.on_result(RegisterTaskResult::kDuplicate);
        }
        return;
    }
    PendingTask pending_task{
        .task =
            {
                .id = std::move(command.task_id),
                .trigger_at = command.trigger_at,
                .status = TaskStatus::kPending,
                .created_at = applied_at,
                .updated_at = applied_at,
            },
        .callback = std::move(command.callback),
    };
    auto used_task_id = pending_task.task.id.Value();
    used_task_ids_.reserve(used_task_ids_.size() + 1);
    const auto insertion_point = std::lower_bound(pending_tasks_.begin(), pending_tasks_.end(), pending_task,
                                                  [](const PendingTask& lhs, const PendingTask& rhs) {
                                                      if (lhs.task.trigger_at != rhs.task.trigger_at) {
                                                          return lhs.task.trigger_at < rhs.task.trigger_at;
                                                      }
                                                      return lhs.task.id.Value() < rhs.task.id.Value();
                                                  });
    const auto inserted_task = pending_tasks_.insert(insertion_point, std::move(pending_task));
    pending_tasks_by_id_.emplace(used_task_id, inserted_task);
    used_task_ids_.push_back(std::move(used_task_id));
    if (command.on_result) {
        command.on_result(RegisterTaskResult::kRegistered);
    }
}

void InMemoryTimingTaskRunner::ApplyCancelTask(CancelTaskCommand command, TriggerAt applied_at) {
    const auto task_by_id = pending_tasks_by_id_.find(command.task_id.Value());
    if (task_by_id != pending_tasks_by_id_.end()) {
        const auto task = task_by_id->second;
        terminal_tasks_.reserve(terminal_tasks_.size() + 1);
        task->task.status = TaskStatus::kCancelled;
        task->task.updated_at = applied_at;
        terminal_tasks_.push_back(std::move(task->task));
        pending_tasks_.erase(task);
        pending_tasks_by_id_.erase(task_by_id);
        if (command.on_result) {
            command.on_result(CancelTaskResult::kCancelled);
        }
        return;
    }

    if (command.on_result) {
        command.on_result(CancelTaskResult::kNotFound);
    }
}

RunDueTasksResult InMemoryTimingTaskRunner::RunDueTasks(TriggerAt now) {
    ProcessPendingCommands(now);
    const auto due_end = std::upper_bound(
        pending_tasks_.begin(), pending_tasks_.end(), now,
        [](TriggerAt boundary, const PendingTask& pending) { return boundary < pending.task.trigger_at; });
    std::vector<std::string> due_task_ids;
    due_task_ids.reserve(static_cast<size_t>(std::distance(pending_tasks_.begin(), due_end)));
    for (auto due_task = pending_tasks_.begin(); due_task != due_end; ++due_task) {
        due_task_ids.push_back(due_task->task.id.Value());
    }
    terminal_tasks_.reserve(terminal_tasks_.size() + due_task_ids.size());
    struct BatchGuard {
        InMemoryTimingTaskRunner& runner;
        InMemoryTimingTaskRunner* previous_callback_runner;
        ~BatchGuard() {
            runner.callback_applied_at_.reset();
            active_callback_runner = previous_callback_runner;
        }
    } batch_guard{*this, active_callback_runner};
    size_t processed_count = 0;
    size_t skipped_count = 0;
    for (const auto& due_task_id : due_task_ids) {
        const auto due_task_by_id = pending_tasks_by_id_.find(due_task_id);
        if (due_task_by_id == pending_tasks_by_id_.end()) {
            ++skipped_count;
            continue;
        }
        const auto due_task = due_task_by_id->second;
        auto executing_task = std::move(*due_task);
        pending_tasks_.erase(due_task);
        pending_tasks_by_id_.erase(due_task_by_id);
        executing_task.task.status = TaskStatus::kExecuting;
        executing_task.task.updated_at = now;
        callback_applied_at_ = now;
        active_callback_runner = this;
        executing_task.callback(executing_task.task.id, executing_task.task.trigger_at);
        active_callback_runner = batch_guard.previous_callback_runner;
        callback_applied_at_.reset();
        executing_task.task.status = TaskStatus::kCompleted;
        executing_task.task.updated_at = now;
        terminal_tasks_.push_back(std::move(executing_task.task));
        ++processed_count;
    }
    return {
        .processed_count = processed_count,
        .skipped_count = skipped_count,
        .next_wake_at = NextWakeAt(),
    };
}

std::optional<TriggerAt> InMemoryTimingTaskRunner::NextWakeAt() const {
    if (pending_tasks_.empty()) {
        return std::nullopt;
    }
    return pending_tasks_.front().task.trigger_at;
}

}  // namespace voicelife::timing
