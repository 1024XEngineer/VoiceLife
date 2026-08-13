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
    registration_commands_.push_back(std::move(command));
    return CommandAcceptance::kAccepted;
}

size_t InMemoryTimingTaskRunner::ProcessPendingRegistrations(TriggerAt registered_at) {
    if (processing_registrations_) {
        return 0;
    }
    processing_registrations_ = true;
    struct ProcessingGuard {
        bool& processing;
        ~ProcessingGuard() { processing = false; }
    } processing_guard{processing_registrations_};

    const auto processed_count = registration_commands_.size();
    for (size_t index = 0; index < processed_count; ++index) {
        auto command = std::move(registration_commands_.front());
        registration_commands_.pop_front();
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
                    .created_at = registered_at,
                    .updated_at = registered_at,
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

std::optional<TriggerAt> InMemoryTimingTaskRunner::NextWakeAt() const {
    if (pending_tasks_.empty()) {
        return std::nullopt;
    }
    return pending_tasks_.front().task.trigger_at;
}

}  // namespace voicelife::timing
