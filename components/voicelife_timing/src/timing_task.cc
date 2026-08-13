#include "voicelife/timing/timing_task.h"

#include <algorithm>
#include <utility>

namespace voicelife::timing {

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
    commands_.push_back(std::move(command));
    return CommandAcceptance::kAccepted;
}

CommandAcceptance InMemoryTimingTaskRunner::CancelTask(CancelTaskCommand command) {
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
            const auto task =
                std::find_if(pending_tasks_.begin(), pending_tasks_.end(),
                             [&cancel](const auto& pending) { return pending.task.id == cancel->task_id; });
            if (task == pending_tasks_.end()) {
                if (cancel->on_result) {
                    cancel->on_result(CancelTaskResult::kNotFound);
                }
                continue;
            }
            terminal_tasks_.reserve(terminal_tasks_.size() + 1);
            task->task.status = TaskStatus::kCancelled;
            task->task.updated_at = applied_at;
            terminal_tasks_.push_back(std::move(task->task));
            pending_tasks_.erase(task);
            if (cancel->on_result) {
                cancel->on_result(CancelTaskResult::kCancelled);
            }
            continue;
        }

        auto command = std::move(std::get<RegisterTaskCommand>(pending_command));
        if (std::find(used_task_ids_.begin(), used_task_ids_.end(), command.task_id.Value()) != used_task_ids_.end()) {
            if (command.on_result) {
                command.on_result(RegisterTaskResult::kDuplicate);
            }
            continue;
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
        pending_tasks_.reserve(pending_tasks_.size() + 1);
        used_task_ids_.reserve(used_task_ids_.size() + 1);
        const auto insertion_point = std::lower_bound(pending_tasks_.begin(), pending_tasks_.end(), pending_task,
                                                      [](const PendingTask& lhs, const PendingTask& rhs) {
                                                          if (lhs.task.trigger_at != rhs.task.trigger_at) {
                                                              return lhs.task.trigger_at < rhs.task.trigger_at;
                                                          }
                                                          return lhs.task.id.Value() < rhs.task.id.Value();
                                                      });
        pending_tasks_.insert(insertion_point, std::move(pending_task));
        used_task_ids_.push_back(std::move(used_task_id));
        if (command.on_result) {
            command.on_result(RegisterTaskResult::kRegistered);
        }
    }
    return processed_count;
}

RunDueTasksResult InMemoryTimingTaskRunner::RunDueTasks(TriggerAt now) {
    ProcessPendingCommands(now);
    const auto due_end = std::upper_bound(
        pending_tasks_.begin(), pending_tasks_.end(), now,
        [](TriggerAt boundary, const PendingTask& pending) { return boundary < pending.task.trigger_at; });
    const auto processed_count = static_cast<size_t>(due_end - pending_tasks_.begin());
    terminal_tasks_.reserve(terminal_tasks_.size() + processed_count);
    for (auto due_task = pending_tasks_.begin(); due_task != due_end; ++due_task) {
        due_task->task.status = TaskStatus::kExecuting;
        due_task->task.updated_at = now;
        due_task->callback(due_task->task.id, due_task->task.trigger_at);
        due_task->task.status = TaskStatus::kCompleted;
        due_task->task.updated_at = now;
        terminal_tasks_.push_back(std::move(due_task->task));
    }
    pending_tasks_.erase(pending_tasks_.begin(), due_end);
    return {
        .processed_count = processed_count,
        .skipped_count = 0,
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
