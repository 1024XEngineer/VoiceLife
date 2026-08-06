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
        if (!next_rule_list_failure_.ok()) {
            Status failure = std::move(next_rule_list_failure_);
            next_rule_list_failure_ = Status::Ok();
            return Result<std::vector<timing::ReminderRule>>::Failure(failure.code, failure.message);
        }
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

    Result<int> DisableReminderRule(const std::string& reminder_rule_id, int64_t now) override {
        const auto existing = rules_.find(reminder_rule_id);
        if (existing == rules_.end()) {
            return Result<int>::Failure(ErrorCode::kNotFound, "reminder rule not found");
        }
        if (existing->second.status == timing::ReminderRuleStatus::kDisabled) {
            return Result<int>::Failure(ErrorCode::kConflict, "reminder rule already disabled");
        }
        if (!tasks_.contains(existing->second.task_id)) {
            return Result<int>::Failure(ErrorCode::kConflict, "reminder rule task relation is invalid");
        }

        auto next_rules = rules_;
        auto next_triggers = triggers_;
        auto& rule = next_rules.at(reminder_rule_id);
        rule.status = timing::ReminderRuleStatus::kDisabled;
        rule.updated_at = now;
        rule.deleted_at = now;
        int affected_trigger_count = 0;
        for (auto& [_, trigger] : next_triggers) {
            if (trigger.reminder_rule_id != reminder_rule_id) {
                continue;
            }
            if (trigger.task_id != rule.task_id) {
                return Result<int>::Failure(ErrorCode::kConflict, "reminder trigger task relation is invalid");
            }
            if (trigger.status == timing::ReminderTriggerStatus::kPending && trigger.planned_trigger_at >= now) {
                trigger.status = timing::ReminderTriggerStatus::kCancelled;
                trigger.updated_at = now;
                trigger.last_action_at = now;
                ++affected_trigger_count;
            }
        }
        if (!next_rule_disable_failure_.ok()) {
            Status failure = std::move(next_rule_disable_failure_);
            next_rule_disable_failure_ = Status::Ok();
            return Result<int>::Failure(failure.code, failure.message);
        }
        rules_ = std::move(next_rules);
        triggers_ = std::move(next_triggers);
        return Result<int>::Success(affected_trigger_count);
    }

    void FailNextRuleDisable(Status failure) { next_rule_disable_failure_ = std::move(failure); }

    void AddReminderRule(timing::ReminderRule rule) { rules_.insert_or_assign(rule.id, std::move(rule)); }

    void AddReminderTrigger(timing::ReminderTrigger trigger) {
        triggers_.insert_or_assign(trigger.id, std::move(trigger));
    }

    Result<timing::ReminderTrigger> FindReminderTrigger(const std::string& trigger_id) const {
        const auto found = triggers_.find(trigger_id);
        if (found == triggers_.end()) {
            return Result<timing::ReminderTrigger>::Failure(ErrorCode::kNotFound, "reminder trigger not found");
        }
        return Result<timing::ReminderTrigger>::Success(found->second);
    }

    Status UpsertRules(const timing::TimingTaskId& task_id, const std::vector<timing::ReminderRule>& rules) override {
        if (!tasks_.contains(task_id)) {
            return Status::Error(ErrorCode::kNotFound, "task not found");
        }
        if (!next_rule_upsert_failure_.ok()) {
            Status failure = std::move(next_rule_upsert_failure_);
            next_rule_upsert_failure_ = Status::Ok();
            return failure;
        }

        auto next_rules = rules_;
        std::unordered_set<std::string> incoming_rule_ids;
        for (const auto& rule : rules) {
            const auto existing = next_rules.find(rule.id);
            if (rule.id.empty() || rule.task_id != task_id || !incoming_rule_ids.insert(rule.id).second ||
                (existing != next_rules.end() && existing->second.task_id != task_id)) {
                return Status::Error(ErrorCode::kConflict, "invalid reminder rule");
            }
            next_rules.insert_or_assign(rule.id, rule);
        }

        int active_on_time_strong_count = 0;
        for (const auto& [_, rule] : next_rules) {
            if (rule.task_id == task_id && rule.status == timing::ReminderRuleStatus::kActive &&
                rule.type == timing::ReminderType::kStrong && rule.offset_minutes == 0) {
                ++active_on_time_strong_count;
            }
        }
        if (active_on_time_strong_count > 1) {
            return Status::Error(ErrorCode::kConflict, "multiple active on-time strong rules");
        }
        rules_ = std::move(next_rules);
        return Status::Ok();
    }

    void FailNextRuleUpsert(Status failure) { next_rule_upsert_failure_ = std::move(failure); }

    void FailNextRuleList(Status failure) { next_rule_list_failure_ = std::move(failure); }

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
    Status next_rule_list_failure_ = Status::Ok();
    Status next_rule_upsert_failure_ = Status::Ok();
    std::unordered_map<timing::TimingTaskId, timing::TimingTask> tasks_;
    std::unordered_map<std::string, timing::ReminderRule> rules_;
    std::unordered_map<std::string, timing::ReminderTrigger> triggers_;
    Status next_rule_disable_failure_ = Status::Ok();
    std::unordered_map<std::string, timing::TimerInstance> instances_;
};

}  // namespace voicelife::test
