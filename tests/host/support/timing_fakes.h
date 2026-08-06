#pragma once

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "voicelife/timing/timing_task_store.h"

namespace voicelife::test {

class InMemoryTimingTaskStore final : public timing::TimingTaskStorePort {
   public:
    void FailNextUpdate(Status status) { next_update_failure_ = std::move(status); }

    Status RegisterTaskWithRules(const timing::TimingTask& task,
                                 const std::vector<timing::ReminderRule>& rules) override {
        if (task.id.empty() || task.request_id.empty()) {
            return Status::Error(ErrorCode::kInvalidArgument, "task or request id is empty");
        }
        if (tasks_.contains(task.id)) {
            return Status::Error(ErrorCode::kConflict, "task exists");
        }
        for (const auto& [_, existing] : tasks_) {
            if (existing.request_id == task.request_id || existing.schedule_id == task.schedule_id) {
                return Status::Error(ErrorCode::kConflict, "request or schedule exists");
            }
        }

        std::unordered_set<std::string> incoming_rule_ids;
        for (const auto& rule : rules) {
            if (rule.id.empty() || rule.task_id != task.id || rules_.contains(rule.id) ||
                !incoming_rule_ids.insert(rule.id).second) {
                return Status::Error(ErrorCode::kConflict, "invalid reminder rule");
            }
        }

        tasks_.emplace(task.id, task);
        for (const auto& rule : rules) {
            rules_.emplace(rule.id, rule);
        }
        return Status::Ok();
    }

    Result<timing::TimingTask> FindTaskByRequestId(const std::string& request_id) override {
        if (request_id.empty()) {
            return Result<timing::TimingTask>::Failure(ErrorCode::kInvalidArgument, "request id is empty");
        }
        for (const auto& [_, task] : tasks_) {
            if (task.request_id == request_id) {
                return Result<timing::TimingTask>::Success(task);
            }
        }
        return Result<timing::TimingTask>::Failure(ErrorCode::kNotFound, "request not found");
    }

    Status UpdateTaskWithInstances(const timing::TimingTaskUpdateWrite& update) override {
        if (next_update_failure_.has_value()) {
            const Status failure = *next_update_failure_;
            next_update_failure_.reset();
            return failure;
        }
        if (update.task.id.empty() || !tasks_.contains(update.task.id)) {
            return Status::Error(ErrorCode::kNotFound, "task not found");
        }

        std::unordered_set<std::string> incoming_instance_ids;
        for (const auto& instance : update.upsert_instances) {
            if (instance.id.empty() || instance.task_id != update.task.id ||
                !incoming_instance_ids.insert(instance.id).second ||
                (instances_.contains(instance.id) && instances_.at(instance.id).task_id != update.task.id)) {
                return Status::Error(ErrorCode::kConflict, "invalid timer instance");
            }
        }

        tasks_[update.task.id] = update.task;
        for (const auto& instance : update.upsert_instances) {
            instances_[instance.id] = instance;
        }
        return Status::Ok();
    }

    Result<timing::TimingTask> FindTask(const timing::TimingTaskId& task_id) override {
        const auto found = tasks_.find(task_id);
        if (found == tasks_.end()) {
            return Result<timing::TimingTask>::Failure(ErrorCode::kNotFound, "task not found");
        }
        return Result<timing::TimingTask>::Success(found->second);
    }

    Result<std::vector<timing::ReminderRule>> ListRules(const timing::TimingTaskId& task_id) override {
        std::vector<timing::ReminderRule> result;
        for (const auto& [_, rule] : rules_) {
            if (rule.task_id == task_id) {
                result.push_back(rule);
            }
        }
        std::sort(result.begin(), result.end(),
                  [](const auto& left, const auto& right) { return left.offset_minutes < right.offset_minutes; });
        return Result<std::vector<timing::ReminderRule>>::Success(std::move(result));
    }

    Result<std::vector<timing::TimerInstance>> ListInstances(const timing::TimingTaskId& task_id) override {
        std::vector<timing::TimerInstance> result;
        for (const auto& [_, instance] : instances_) {
            if (instance.task_id == task_id && instance.deleted_at == 0) {
                result.push_back(instance);
            }
        }
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            if (left.planned_at == right.planned_at) {
                return left.id < right.id;
            }
            return left.planned_at < right.planned_at;
        });
        return Result<std::vector<timing::TimerInstance>>::Success(std::move(result));
    }

   private:
    std::optional<Status> next_update_failure_{};
    std::unordered_map<timing::TimingTaskId, timing::TimingTask> tasks_;
    std::unordered_map<std::string, timing::ReminderRule> rules_;
    std::unordered_map<std::string, timing::TimerInstance> instances_;
};

}  // namespace voicelife::test
