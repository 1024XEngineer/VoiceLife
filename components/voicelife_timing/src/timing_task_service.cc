#include "voicelife/timing/timing_task_service.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "voicelife/timing/recurrence_policy.h"

namespace voicelife::timing {
namespace {

template <typename T>
Result<T> Failure(const Status& status) {
    return Result<T>::Failure(status.code, status.message);
}

bool ValidPage(int page, int size) { return page > 0 && size > 0 && size <= 100; }

}  // namespace

Result<TimerTaskResult> DefaultTimingTaskService::RegisterTimerTask(const RegisterTimerTaskCommand& command) {
    if (command.schedule_id.empty() || command.start_at <= 0 || command.time_zone.empty()) {
        return Result<TimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "注册任务缺少日程、开始时间或时区");
    }
    const int64_t now = clock_.Now();
    TimingTask task{
        .id = ids_.Next("task"),
        .schedule_id = command.schedule_id,
        .next_trigger_at = command.start_at,
        .time_zone = command.time_zone,
        .recurrence = command.recurrence,
        .status = TimingTaskStatus::kActive,
        .created_at = now,
        .updated_at = now,
    };
    if (task.recurrence.start_at == 0) task.recurrence.start_at = command.start_at;
    task.recurrence.time_zone = command.time_zone;
    const Status recurrence_valid = RecurrencePolicy().Validate(task.recurrence);
    if (!recurrence_valid.ok()) return Failure<TimerTaskResult>(recurrence_valid);
    std::vector<ReminderRule> rules{
        {.id = ids_.Next("rule"), .task_id = task.id, .type = ReminderType::kWeak, .offset_minutes = -10,
         .channel = "voice", .source = "system_default", .created_at = now, .updated_at = now},
        {.id = ids_.Next("rule"), .task_id = task.id, .type = ReminderType::kStrong, .offset_minutes = 0,
         .max_snooze_count = 3, .snooze_interval_minutes = 5, .channel = "voice",
         .source = "system_default", .created_at = now, .updated_at = now},
    };
    const Status saved = store_.RegisterTaskWithRules(task, rules);
    if (!saved.ok()) return Failure<TimerTaskResult>(saved);
    return Result<TimerTaskResult>::Success({task.id, task.status, task.next_trigger_at});
}

Result<UpdateTimerTaskResult> DefaultTimingTaskService::UpdateTimerTask(const UpdateTimerTaskCommand& command) {
    if (command.task_id.empty() || (command.start_at <= 0 && !command.recurrence.has_value())) {
        return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "更新任务缺少任务标识或更新字段");
    }
    auto found = store_.FindTask(command.task_id);
    if (!found.ok()) return Failure<UpdateTimerTaskResult>(found.status);
    if (!command.schedule_id.empty() && command.schedule_id != found.value->schedule_id) {
        return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kConflict, "任务与日程不匹配");
    }
    if (command.scope == ChangeScope::kSingle) {
        if (command.start_at <= 0) {
            return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "单次更新需要开始时间");
        }
        if (command.instance_id.empty() && command.target_occurrence_at <= 0) {
            return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "单次更新缺少目标 occurrence");
        }
        std::optional<TimerInstance> existing;
        if (!command.instance_id.empty()) {
            auto result = store_.FindInstance(command.instance_id);
            if (!result.ok()) return Failure<UpdateTimerTaskResult>(result.status);
            existing = *result.value;
            if (!existing.has_value()) return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kNotFound, "目标实例不存在");
        } else {
            auto result = store_.FindInstanceByOccurrence(command.task_id, command.target_occurrence_at);
            if (!result.ok()) return Failure<UpdateTimerTaskResult>(result.status);
            existing = *result.value;
        }
        const int64_t now = clock_.Now();
        TimerInstance instance = existing.value_or(TimerInstance{
            .id = ids_.Next("instance"), .task_id = command.task_id,
            .planned_at = command.target_occurrence_at, .created_at = now, .updated_at = now});
        instance.actual_trigger_at = command.start_at;
        instance.status = TimerInstanceStatus::kModified;
        if (instance.task_id != command.task_id) return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kConflict, "实例不属于目标任务");
        instance.override_fields.start_at = command.start_at;
        instance.last_action_at = now;
        instance.updated_at = now;
        const Status saved = store_.UpsertInstance(instance);
        if (!saved.ok()) return Failure<UpdateTimerTaskResult>(saved);
        return Result<UpdateTimerTaskResult>::Success({command.task_id, found.value->status,
                                                        found.value->next_trigger_at, instance.id, 1});
    }
    if (command.scope == ChangeScope::kFuture && command.effective_from <= 0) {
        return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kInvalidArgument,
                                                       "future 更新缺少 effective_from");
    }
    const int64_t new_start = command.start_at > 0 ? command.start_at : command.recurrence->start_at;
    if (new_start <= 0) return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "周期更新缺少锚点时间");
    RecurrenceRule updated_recurrence = command.recurrence.value_or(found.value->recurrence);
    updated_recurrence.start_at = new_start;
    if (updated_recurrence.time_zone.empty()) updated_recurrence.time_zone = found.value->time_zone;
    const Status recurrence_valid = RecurrencePolicy().Validate(updated_recurrence);
    if (!recurrence_valid.ok()) return Failure<UpdateTimerTaskResult>(recurrence_valid);
    found.value->updated_at = clock_.Now();
    const TimingEvent event{.event_type = TimingEventType::kTaskUpdated, .event_id = ids_.Next("event"),
                            .task_id = found.value->id, .schedule_id = found.value->schedule_id,
                            .trigger_at = new_start, .status = TimingEventStatus::kActive,
                            .occurred_at = found.value->updated_at};
    if (command.scope == ChangeScope::kFuture) {
        if (new_start < command.effective_from) {
            return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kInvalidArgument,
                                                           "future 规则开始时间早于生效边界");
        }
        found.value->pending_recurrence = updated_recurrence;
        found.value->pending_effective_from = command.effective_from;
        if (found.value->next_trigger_at >= command.effective_from) {
            found.value->recurrence = updated_recurrence;
            found.value->next_trigger_at = new_start;
            found.value->pending_recurrence.reset();
            found.value->pending_effective_from = 0;
        }
        auto updated = store_.ApplyFutureUpdate(*found.value, command.effective_from, found.value->updated_at, event);
        if (!updated.ok()) return Failure<UpdateTimerTaskResult>(updated.status);
        return Result<UpdateTimerTaskResult>::Success({found.value->id, found.value->status,
                                                        found.value->next_trigger_at, {}, *updated.value});
    }
    found.value->recurrence = updated_recurrence;
    found.value->next_trigger_at = new_start;
    found.value->pending_recurrence.reset();
    found.value->pending_effective_from = 0;
    const Status saved = store_.UpdateTaskWithEvent(*found.value, event);
    if (!saved.ok()) return Failure<UpdateTimerTaskResult>(saved);
    return Result<UpdateTimerTaskResult>::Success({found.value->id, found.value->status,
                                                    found.value->next_trigger_at, {}, 0});
}

Result<CancelTimerTaskResult> DefaultTimingTaskService::CancelTimerTask(const CancelTimerTaskCommand& command) {
    if (command.task_id.empty()) return Result<CancelTimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "取消任务缺少任务标识");
    auto found = store_.FindTask(command.task_id);
    if (!found.ok()) return Failure<CancelTimerTaskResult>(found.status);
    if (!command.schedule_id.empty() && command.schedule_id != found.value->schedule_id) {
        return Result<CancelTimerTaskResult>::Failure(ErrorCode::kConflict, "任务与日程不匹配");
    }
    if (command.scope == ChangeScope::kSingle) {
        const int64_t planned = command.target_occurrence_at;
        if (command.instance_id.empty() && planned <= 0) return Result<CancelTimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "单次取消缺少目标 occurrence");
        auto lookup = command.instance_id.empty() ? store_.FindInstanceByOccurrence(command.task_id, planned)
                                                   : store_.FindInstance(command.instance_id);
        if (!lookup.ok()) return Failure<CancelTimerTaskResult>(lookup.status);
        if (!command.instance_id.empty() && !lookup.value->has_value()) return Result<CancelTimerTaskResult>::Failure(ErrorCode::kNotFound, "目标实例不存在");
        const int64_t now = clock_.Now();
        TimerInstance instance = lookup.value->value_or(TimerInstance{.id = ids_.Next("instance"), .task_id = command.task_id, .planned_at = planned, .created_at = now});
        instance.status = TimerInstanceStatus::kSkipped;
        if (instance.task_id != command.task_id) return Result<CancelTimerTaskResult>::Failure(ErrorCode::kConflict, "实例不属于目标任务");
        instance.last_action_at = now;
        instance.updated_at = now;
        const Status saved = store_.UpsertInstance(instance);
        if (!saved.ok()) return Failure<CancelTimerTaskResult>(saved);
        return Result<CancelTimerTaskResult>::Success({command.task_id, instance.id, found.value->status, 1});
    }
    found.value->updated_at = clock_.Now();
    const TimingEvent event{.event_type = TimingEventType::kTaskCancelled, .event_id = ids_.Next("event"),
                            .task_id = found.value->id, .schedule_id = found.value->schedule_id,
                            .trigger_at = found.value->updated_at, .status = TimingEventStatus::kTerminated,
                            .occurred_at = found.value->updated_at};
    if (command.scope == ChangeScope::kFuture) {
        if (command.effective_from <= 0) return Result<CancelTimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "future 取消缺少 effective_from");
        found.value->effective_until = command.effective_from;
        auto cancelled = store_.CancelFuture(*found.value, command.effective_from, found.value->updated_at, event);
        if (!cancelled.ok()) return Failure<CancelTimerTaskResult>(cancelled.status);
        return Result<CancelTimerTaskResult>::Success({found.value->id, {}, found.value->status, *cancelled.value});
    }
    found.value->status = TimingTaskStatus::kTerminated;
    found.value->next_trigger_at = 0;
    const Status saved = store_.UpdateTaskWithEvent(*found.value, event);
    if (!saved.ok()) return Failure<CancelTimerTaskResult>(saved);
    return Result<CancelTimerTaskResult>::Success({found.value->id, {}, found.value->status, 0});
}

Result<std::vector<ReminderRule>> DefaultTimingTaskService::UpsertReminderRules(
    const std::string& task_id, std::vector<ReminderRule> rules) {
    if (task_id.empty() || rules.empty()) return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInvalidArgument, "提醒规则不能为空");
    auto task = store_.FindTask(task_id);
    if (!task.ok()) return Failure<std::vector<ReminderRule>>(task.status);
    auto current = store_.ListRules(task_id);
    if (!current.ok()) return Failure<std::vector<ReminderRule>>(current.status);
    int strong_at_start = 0;
    for (const auto& r : *current.value) if (r.status == ReminderRuleStatus::kActive && r.type == ReminderType::kStrong && r.offset_minutes == 0) ++strong_at_start;
    const int64_t now = clock_.Now();
    for (auto& rule : rules) {
        if (rule.type == ReminderType::kWeak && (rule.max_snooze_count != 0 || rule.snooze_interval_minutes != 0)) return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInvalidArgument, "弱提醒不支持 snooze");
        if (rule.type == ReminderType::kStrong && rule.offset_minutes == 0 && rule.status == ReminderRuleStatus::kActive && rule.id.empty()) ++strong_at_start;
        if (rule.id.empty()) rule.id = ids_.Next("rule");
        rule.task_id = task_id;
        if (rule.created_at == 0) rule.created_at = now;
        rule.updated_at = now;
    }
    if (strong_at_start > 1) return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kConflict, "每个任务只能有一条开始时强提醒");
    const Status saved = store_.UpsertRules(task_id, rules);
    if (!saved.ok()) return Failure<std::vector<ReminderRule>>(saved);
    return store_.ListRules(task_id);
}

Result<DeleteReminderRuleResult> DefaultTimingTaskService::DeleteReminderRule(const std::string& rule_id) {
    if (rule_id.empty()) return Result<DeleteReminderRuleResult>::Failure(ErrorCode::kInvalidArgument, "提醒规则标识不能为空");
    auto found = store_.FindRule(rule_id);
    if (!found.ok()) return Failure<DeleteReminderRuleResult>(found.status);
    if (!found.value->has_value()) return Result<DeleteReminderRuleResult>::Failure(ErrorCode::kNotFound, "提醒规则不存在");
    auto disabled = store_.DisableRuleAndCancelPendingTriggers(rule_id, clock_.Now());
    if (!disabled.ok()) return Failure<DeleteReminderRuleResult>(disabled.status);
    return Result<DeleteReminderRuleResult>::Success({rule_id, ReminderRuleStatus::kDisabled, *disabled.value});
}

Result<CalendarView> DefaultTimingTaskService::ListCalendarView(const CalendarViewQuery& query) {
    if (query.range_start <= 0 || query.range_end <= query.range_start || !ValidPage(query.page, query.page_size)) return Result<CalendarView>::Failure(ErrorCode::kInvalidArgument, "日历查询范围或分页无效");
    auto tasks = store_.ListTasks();
    if (!tasks.ok()) return Failure<CalendarView>(tasks.status);
    std::vector<CalendarOccurrence> values;
    RecurrencePolicy recurrence;
    for (const auto& task : *tasks.value) {
        if (task.status != TimingTaskStatus::kActive || (!query.schedule_id.empty() && task.schedule_id != query.schedule_id)) continue;
        const int64_t task_end = task.effective_until > 0 ? std::min(query.range_end, task.effective_until)
                                                          : query.range_end;
        std::vector<int64_t> expanded;
        const int64_t current_end = task.pending_recurrence
                                        ? std::min(task_end, task.pending_effective_from)
                                        : task_end;
        if (current_end > query.range_start) {
            auto current = recurrence.Expand(task.recurrence, query.range_start, current_end);
            if (!current.ok()) return Failure<CalendarView>(current.status);
            expanded.insert(expanded.end(), current.value->begin(), current.value->end());
        }
        if (task.pending_recurrence && task_end > task.pending_effective_from) {
            auto pending = recurrence.Expand(*task.pending_recurrence,
                                             std::max(query.range_start, task.pending_effective_from),
                                             task_end);
            if (!pending.ok()) return Failure<CalendarView>(pending.status);
            expanded.insert(expanded.end(), pending.value->begin(), pending.value->end());
        }
        std::unordered_set<int64_t> seen_occurrences;
        for (int64_t at : expanded) {
            seen_occurrences.insert(at);
            auto existing = store_.FindInstanceByOccurrence(task.id, at);
            if (!existing.ok()) return Failure<CalendarView>(existing.status);
            const auto instance = *existing.value;
            if (instance && instance->deleted_at != 0) continue;
            const TimerInstanceStatus status = instance ? instance->status : TimerInstanceStatus::kPending;
            if (query.status && status != *query.status) continue;
            values.push_back({task.id + "@" + std::to_string(at), task.schedule_id, task.id,
                              instance ? instance->id : std::string{}, {}, at,
                              instance ? instance->planned_end_at : 0,
                              instance ? instance->override_fields.start_at.value_or(at) : at,
                              status, task.recurrence.frequency != RecurrenceFrequency::kNone,
                              instance.has_value(), instance ? instance->override_fields : InstanceOverrides{}});
        }
        auto materialized = store_.ListInstances(task.id);
        if (!materialized.ok()) return Failure<CalendarView>(materialized.status);
        for (const auto& instance : *materialized.value) {
            if (instance.planned_at < query.range_start || instance.planned_at >= task_end ||
                seen_occurrences.contains(instance.planned_at) ||
                (query.status && instance.status != *query.status)) {
                continue;
            }
            values.push_back({
                task.id + "@" + std::to_string(instance.planned_at), task.schedule_id, task.id,
                instance.id, {}, instance.planned_at, instance.planned_end_at,
                instance.override_fields.start_at.value_or(
                    instance.actual_trigger_at > 0 ? instance.actual_trigger_at : instance.planned_at),
                instance.status, task.recurrence.frequency != RecurrenceFrequency::kNone,
                true, instance.override_fields,
            });
        }
    }
    std::sort(values.begin(), values.end(), [&](const auto& a, const auto& b) {
        const int64_t left = query.sort_by == CalendarSortBy::kPlannedStartAt ? a.planned_start_at : a.actual_trigger_at;
        const int64_t right = query.sort_by == CalendarSortBy::kPlannedStartAt ? b.planned_start_at : b.actual_trigger_at;
        return query.sort_order == SortOrder::kAscending ? left < right : left > right;
    });
    const int total = static_cast<int>(values.size());
    const size_t begin = std::min(values.size(), static_cast<size_t>((query.page - 1) * query.page_size));
    const size_t end = std::min(values.size(), begin + static_cast<size_t>(query.page_size));
    return Result<CalendarView>::Success({std::vector<CalendarOccurrence>(values.begin() + begin, values.begin() + end), total, query.page, query.page_size, end < values.size()});
}

Result<ReminderTriggerPage> DefaultTimingTaskService::ListReminderTriggers(const ReminderTriggerQuery& query) {
    const bool range = query.range_start > 0 || query.range_end > 0;
    if ((!ValidPage(query.page, query.page_size)) || (range && (query.range_start <= 0 || query.range_end <= query.range_start)) ||
        (query.task_id.empty() && query.instance_id.empty() && query.schedule_id.empty() && !range)) return Result<ReminderTriggerPage>::Failure(ErrorCode::kInvalidArgument, "提醒查询条件无效");
    auto all = store_.ListTriggers();
    if (!all.ok()) return Failure<ReminderTriggerPage>(all.status);
    std::vector<ReminderTrigger> values;
    for (const auto& trigger : *all.value) {
        if (!query.task_id.empty() && trigger.task_id != query.task_id) continue;
        if (!query.instance_id.empty() && trigger.instance_id != query.instance_id) continue;
        if (query.type && trigger.type != *query.type) continue;
        if (query.status && trigger.status != *query.status) continue;
        if (range && (trigger.actual_trigger_at < query.range_start || trigger.actual_trigger_at >= query.range_end)) continue;
        if (!query.schedule_id.empty()) { auto task = store_.FindTask(trigger.task_id); if (!task.ok() || task.value->schedule_id != query.schedule_id) continue; }
        values.push_back(trigger);
    }
    std::sort(values.begin(), values.end(), [&](const auto& a, const auto& b) {
        auto key = [&](const ReminderTrigger& value) {
            if (query.sort_by == TriggerSortBy::kPlannedTriggerAt) return value.planned_trigger_at;
            if (query.sort_by == TriggerSortBy::kCreatedAt) return value.created_at;
            return value.actual_trigger_at;
        };
        return query.sort_order == SortOrder::kAscending ? key(a) < key(b) : key(a) > key(b);
    });
    const int total = static_cast<int>(values.size());
    const size_t begin = std::min(values.size(), static_cast<size_t>((query.page - 1) * query.page_size));
    const size_t end = std::min(values.size(), begin + static_cast<size_t>(query.page_size));
    return Result<ReminderTriggerPage>::Success({std::vector<ReminderTrigger>(values.begin() + begin, values.begin() + end), total, query.page, query.page_size, end < values.size()});
}

Result<ReminderTrigger> DefaultTimingTaskService::SnoozeReminderTrigger(const SnoozeReminderTriggerCommand& command) {
    if (command.reminder_trigger_id.empty() || command.delay_minutes <= 0) return Result<ReminderTrigger>::Failure(ErrorCode::kInvalidArgument, "推迟参数无效");
    auto found = store_.FindTrigger(command.reminder_trigger_id);
    if (!found.ok()) return Failure<ReminderTrigger>(found.status);
    if (!found.value->has_value()) return Result<ReminderTrigger>::Failure(ErrorCode::kNotFound, "提醒触发不存在");
    auto trigger = found.value->value();
    if (trigger.type != ReminderType::kStrong || (trigger.status != ReminderTriggerStatus::kTriggered && trigger.status != ReminderTriggerStatus::kSnoozed)) return Result<ReminderTrigger>::Failure(ErrorCode::kConflict, "当前提醒不允许推迟");
    if (trigger.snooze_count >= trigger.max_snooze_count) return Result<ReminderTrigger>::Failure(ErrorCode::kConflict, "已达到最大推迟次数");
    trigger.actual_trigger_at = clock_.Now() + command.delay_minutes * 60;
    trigger.status = ReminderTriggerStatus::kSnoozed;
    ++trigger.snooze_count;
    trigger.updated_at = clock_.Now();
    auto task = store_.FindTask(trigger.task_id);
    if (!task.ok()) return Failure<ReminderTrigger>(task.status);
    const Status queued = store_.UpdateTriggerWithEvent(trigger, {
        .event_type = TimingEventType::kReminderSnoozed, .event_id = ids_.Next("event"),
        .task_id = trigger.task_id, .instance_id = trigger.instance_id,
        .reminder_rule_id = trigger.reminder_rule_id, .reminder_trigger_id = trigger.id,
        .schedule_id = task.value->schedule_id, .trigger_at = trigger.actual_trigger_at,
        .status = TimingEventStatus::kSnoozed, .occurred_at = clock_.Now(),
    });
    if (!queued.ok()) return Failure<ReminderTrigger>(queued);
    return Result<ReminderTrigger>::Success(trigger);
}

Result<ReminderTrigger> DefaultTimingTaskService::DismissReminderTrigger(const std::string& id) {
    if (id.empty()) return Result<ReminderTrigger>::Failure(ErrorCode::kInvalidArgument, "提醒触发标识不能为空");
    auto found = store_.FindTrigger(id);
    if (!found.ok()) return Failure<ReminderTrigger>(found.status);
    if (!found.value->has_value()) return Result<ReminderTrigger>::Failure(ErrorCode::kNotFound, "提醒触发不存在");
    auto trigger = found.value->value();
    if (trigger.type != ReminderType::kStrong || (trigger.status != ReminderTriggerStatus::kTriggered && trigger.status != ReminderTriggerStatus::kSnoozed)) return Result<ReminderTrigger>::Failure(ErrorCode::kConflict, "当前提醒不允许关闭");
    trigger.status = ReminderTriggerStatus::kDismissed;
    trigger.updated_at = clock_.Now();
    auto task = store_.FindTask(trigger.task_id);
    if (!task.ok()) return Failure<ReminderTrigger>(task.status);
    const Status queued = store_.UpdateTriggerWithEvent(trigger, {
        .event_type = TimingEventType::kReminderDismissed, .event_id = ids_.Next("event"),
        .task_id = trigger.task_id, .instance_id = trigger.instance_id,
        .reminder_rule_id = trigger.reminder_rule_id, .reminder_trigger_id = trigger.id,
        .schedule_id = task.value->schedule_id, .trigger_at = clock_.Now(),
        .status = TimingEventStatus::kDismissed, .occurred_at = clock_.Now(),
    });
    if (!queued.ok()) return Failure<ReminderTrigger>(queued);
    return Result<ReminderTrigger>::Success(trigger);
}

}  // namespace voicelife::timing
