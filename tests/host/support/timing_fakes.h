#pragma once

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "voicelife/timing/timing_task_store.h"

namespace voicelife::test {

class InMemoryTimingTaskStore final : public timing::TimingTaskStorePort {
   public:
    Status RegisterTaskWithRules(const timing::TimingTask& task, const std::vector<timing::ReminderRule>& rules) override {
        if (tasks_.contains(task.id)) return Status::Error(ErrorCode::kConflict, "task exists");
        for (const auto& rule : rules) {
            if (rule.task_id != task.id || rules_.contains(rule.id)) {
                return Status::Error(ErrorCode::kConflict, "invalid rule ownership");
            }
        }
        tasks_[task.id] = task;
        for (const auto& rule : rules) rules_[rule.id] = rule;
        return Status::Ok();
    }
    Result<timing::TimingTask> FindTask(const std::string& id) override {
        const auto it = tasks_.find(id); return it == tasks_.end() ? Result<timing::TimingTask>::Failure(ErrorCode::kNotFound, "task not found") : Result<timing::TimingTask>::Success(it->second);
    }
    Status UpdateTask(const timing::TimingTask& task) override { tasks_[task.id] = task; return Status::Ok(); }
    Status UpdateTaskWithEvent(const timing::TimingTask& task, const timing::TimingEvent& event) override { tasks_[task.id] = task; events_[event.event_id] = event; return Status::Ok(); }
    Result<std::vector<timing::TimingTask>> ListTasks() override { std::vector<timing::TimingTask> out; for (const auto& [_, v] : tasks_) out.push_back(v); return Result<std::vector<timing::TimingTask>>::Success(std::move(out)); }
    Result<std::vector<timing::TimingTask>> ListDueTasks(int64_t now) override { std::vector<timing::TimingTask> out; for (const auto& [_, v] : tasks_) if (v.status == timing::TimingTaskStatus::kActive && v.next_trigger_at > 0 && v.next_trigger_at <= now) out.push_back(v); return Result<std::vector<timing::TimingTask>>::Success(std::move(out)); }
    Status MaterializeOccurrence(const timing::TimerInstance& instance, const std::vector<timing::ReminderTrigger>& values, const timing::TimingTask& task, const timing::TimingEvent& event) override { auto existing = FindInstanceByOccurrence(instance.task_id, instance.planned_at); if (!existing.ok()) return existing.status; if (existing.value->has_value() && existing.value->value().id != instance.id) return Status::Error(ErrorCode::kConflict, "occurrence exists"); instances_[instance.id] = instance; for (const auto& value : values) triggers_[value.id] = value; tasks_[task.id] = task; events_.try_emplace(event.event_id, event); return Status::Ok(); }
    Status UpsertInstance(const timing::TimerInstance& value) override { instances_[value.id] = value; return Status::Ok(); }
    Result<std::optional<timing::TimerInstance>> FindInstance(const std::string& id) override { const auto it = instances_.find(id); return Result<std::optional<timing::TimerInstance>>::Success(it == instances_.end() || it->second.deleted_at != 0 ? std::nullopt : std::optional(it->second)); }
    Result<std::optional<timing::TimerInstance>> FindInstanceByOccurrence(const std::string& task, int64_t at) override { for (const auto& [_, v] : instances_) if (v.task_id == task && v.planned_at == at && v.deleted_at == 0) return Result<std::optional<timing::TimerInstance>>::Success(v); return Result<std::optional<timing::TimerInstance>>::Success(std::nullopt); }
    Result<std::vector<timing::TimerInstance>> ListInstances(const std::string& task) override { std::vector<timing::TimerInstance> out; for (const auto& [_, v] : instances_) if (v.task_id == task && v.deleted_at == 0) out.push_back(v); return Result<std::vector<timing::TimerInstance>>::Success(std::move(out)); }
    Result<int> ApplyFutureUpdate(const timing::TimingTask& task, int64_t from, int64_t now, const timing::TimingEvent& event) override { int affected = 0; std::unordered_set<std::string> future_instances; for (auto& [id, v] : instances_) if (v.task_id == task.id && v.planned_at >= from && v.deleted_at == 0) { future_instances.insert(id); v.deleted_at = now; ++affected; } for (auto& [_, v] : triggers_) if (future_instances.contains(v.instance_id) && (v.status == timing::ReminderTriggerStatus::kPending || v.status == timing::ReminderTriggerStatus::kSnoozed)) v.status = timing::ReminderTriggerStatus::kCancelled; tasks_[task.id] = task; events_[event.event_id] = event; return Result<int>::Success(affected); }
    Result<int> CancelFuture(const timing::TimingTask& task, int64_t from, int64_t now, const timing::TimingEvent& event) override { int affected = 0; std::unordered_set<std::string> future_instances; for (auto& [id, v] : instances_) if (v.task_id == task.id && v.planned_at >= from && v.status != timing::TimerInstanceStatus::kSkipped) { future_instances.insert(id); v.status = timing::TimerInstanceStatus::kSkipped; v.last_action_at = now; ++affected; } for (auto& [_, v] : triggers_) if (future_instances.contains(v.instance_id) && (v.status == timing::ReminderTriggerStatus::kPending || v.status == timing::ReminderTriggerStatus::kSnoozed)) v.status = timing::ReminderTriggerStatus::kCancelled; tasks_[task.id] = task; events_[event.event_id] = event; return Result<int>::Success(affected); }
    Status UpsertRules(const std::string& task_id, const std::vector<timing::ReminderRule>& values) override {
        auto staged = rules_;
        for (const auto& value : values) {
            const auto existing = staged.find(value.id);
            if (value.task_id != task_id || (existing != staged.end() && existing->second.task_id != task_id)) {
                return Status::Error(ErrorCode::kConflict, "rule belongs to another task");
            }
            staged[value.id] = value;
        }
        int active_strong = 0;
        for (const auto& [_, value] : staged) {
            if (value.task_id == task_id && value.deleted_at == 0 &&
                value.status == timing::ReminderRuleStatus::kActive &&
                value.type == timing::ReminderType::kStrong && value.offset_minutes == 0) {
                ++active_strong;
            }
        }
        if (active_strong > 1) return Status::Error(ErrorCode::kConflict, "duplicate active strong rule");
        rules_ = std::move(staged);
        return Status::Ok();
    }
    Result<int> DisableRuleAndCancelPendingTriggers(const std::string& id, int64_t now) override { auto it = rules_.find(id); if (it == rules_.end()) return Result<int>::Failure(ErrorCode::kNotFound, "rule not found"); it->second.status = timing::ReminderRuleStatus::kDisabled; it->second.updated_at = now; int affected = 0; for (auto& [_, trigger] : triggers_) if (trigger.reminder_rule_id == id && (trigger.status == timing::ReminderTriggerStatus::kPending || trigger.status == timing::ReminderTriggerStatus::kSnoozed)) { trigger.status = timing::ReminderTriggerStatus::kCancelled; trigger.updated_at = now; ++affected; } return Result<int>::Success(affected); }
    Result<std::optional<timing::ReminderRule>> FindRule(const std::string& id) override { const auto it = rules_.find(id); return Result<std::optional<timing::ReminderRule>>::Success(it == rules_.end() ? std::nullopt : std::optional(it->second)); }
    Result<std::vector<timing::ReminderRule>> ListRules(const std::string& task) override { std::vector<timing::ReminderRule> out; for (const auto& [_, v] : rules_) if (v.task_id == task) out.push_back(v); std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.offset_minutes < b.offset_minutes; }); return Result<std::vector<timing::ReminderRule>>::Success(std::move(out)); }
    Status UpsertTriggers(const std::vector<timing::ReminderTrigger>& values) override { for (const auto& v : values) triggers_[v.id] = v; return Status::Ok(); }
    Status UpdateTrigger(const timing::ReminderTrigger& value) override { triggers_[value.id] = value; return Status::Ok(); }
    Status UpdateTriggerWithEvent(const timing::ReminderTrigger& value, const timing::TimingEvent& event) override { triggers_[value.id] = value; events_[event.event_id] = event; return Status::Ok(); }
    Result<std::optional<timing::ReminderTrigger>> FindTrigger(const std::string& id) override { const auto it = triggers_.find(id); return Result<std::optional<timing::ReminderTrigger>>::Success(it == triggers_.end() || it->second.deleted_at != 0 ? std::nullopt : std::optional(it->second)); }
    Result<std::vector<timing::ReminderTrigger>> ListTriggers() override { std::vector<timing::ReminderTrigger> out; for (const auto& [_, v] : triggers_) if (v.deleted_at == 0) out.push_back(v); return Result<std::vector<timing::ReminderTrigger>>::Success(std::move(out)); }
    Result<std::vector<timing::ReminderTrigger>> ListDueTriggers(int64_t now) override { std::vector<timing::ReminderTrigger> out; for (const auto& [_, v] : triggers_) if (v.deleted_at == 0 && (v.status == timing::ReminderTriggerStatus::kPending || v.status == timing::ReminderTriggerStatus::kSnoozed) && v.actual_trigger_at <= now) out.push_back(v); return Result<std::vector<timing::ReminderTrigger>>::Success(std::move(out)); }
    Status EnqueueEvent(const timing::TimingEvent& event) override { events_[event.event_id] = event; return Status::Ok(); }
    Result<std::vector<timing::TimingEvent>> ListPendingEvents() override { std::vector<timing::TimingEvent> out; for (const auto& [id, event] : events_) if (!published_.contains(id)) out.push_back(event); return Result<std::vector<timing::TimingEvent>>::Success(std::move(out)); }
    Status MarkEventPublished(const std::string& id) override { if (!events_.contains(id)) return Status::Error(ErrorCode::kNotFound, "event not found"); published_.insert(id); return Status::Ok(); }
   private:
    std::unordered_map<std::string, timing::TimingTask> tasks_;
    std::unordered_map<std::string, timing::TimerInstance> instances_;
    std::unordered_map<std::string, timing::ReminderRule> rules_;
    std::unordered_map<std::string, timing::ReminderTrigger> triggers_;
    std::unordered_map<std::string, timing::TimingEvent> events_;
    std::unordered_set<std::string> published_;
};

}  // namespace voicelife::test
