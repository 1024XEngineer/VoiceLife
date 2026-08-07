#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "voicelife/timing/occurrence_planner.h"
#include "voicelife/timing/timing_task_service.h"

namespace voicelife::timing {
namespace {

constexpr int64_t kSecondsPerDay = 24 * 60 * 60;
constexpr int64_t kSecondsPerMinute = 60;

std::optional<int64_t> AddSeconds(int64_t value, int64_t seconds) {
    if (seconds > 0 && value > std::numeric_limits<int64_t>::max() - seconds) {
        return std::nullopt;
    }
    if (seconds < 0 && value < std::numeric_limits<int64_t>::min() - seconds) {
        return std::nullopt;
    }
    return value + seconds;
}

std::string InstanceId(const TimingTask& task, int64_t planned_at) {
    return task.id + "@" + std::to_string(planned_at);
}

std::string TriggerId(const TimerInstance& instance, const ReminderRule& rule) { return instance.id + "/" + rule.id; }

std::optional<int64_t> NextWindowSeconds(RecurrenceFrequency frequency) {
    switch (frequency) {
        case RecurrenceFrequency::kDay:
            return kSecondsPerDay * 2;
        case RecurrenceFrequency::kWeek:
            return kSecondsPerDay * 14;
        case RecurrenceFrequency::kMonth:
            return kSecondsPerDay * 62;
        case RecurrenceFrequency::kYear:
            return kSecondsPerDay * 366;
        case RecurrenceFrequency::kNone:
            return std::nullopt;
    }
    return std::nullopt;
}

Result<std::optional<int64_t>> FindNextOccurrence(const TimingTask& task, int64_t now) {
    if (task.recurrence.frequency == RecurrenceFrequency::kNone || now == std::numeric_limits<int64_t>::max()) {
        return Result<std::optional<int64_t>>::Success(std::nullopt);
    }

    const auto start = AddSeconds(now, 1);
    auto window = NextWindowSeconds(task.recurrence.frequency);
    if (!start.has_value() || !window.has_value()) {
        return Result<std::optional<int64_t>>::Success(std::nullopt);
    }

    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto end = AddSeconds(*start, *window);
        if (!end.has_value()) {
            const auto planned = PlanOccurrences(task, *start, std::numeric_limits<int64_t>::max());
            if (!planned.ok()) {
                return Result<std::optional<int64_t>>::Failure(planned.status.code, planned.status.message);
            }
            if (!planned.value->empty()) {
                return Result<std::optional<int64_t>>::Success(planned.value->front());
            }
            return Result<std::optional<int64_t>>::Success(std::nullopt);
        }

        const auto planned = PlanOccurrences(task, *start, *end);
        if (!planned.ok()) {
            return Result<std::optional<int64_t>>::Failure(planned.status.code, planned.status.message);
        }
        if (!planned.value->empty()) {
            return Result<std::optional<int64_t>>::Success(planned.value->front());
        }
        if (*window > std::numeric_limits<int64_t>::max() / 2) {
            break;
        }
        *window *= 2;
    }
    return Result<std::optional<int64_t>>::Success(std::nullopt);
}

TimerInstance BuildInstance(const TimingTask& task, int64_t planned_at, int64_t now) {
    return {
        .id = InstanceId(task, planned_at),
        .task_id = task.id,
        .planned_at = planned_at,
        .planned_end_at = planned_at,
        .status = TimerInstanceStatus::kPending,
        .created_at = now,
        .updated_at = now,
    };
}

TimingEvent BuildInstanceEvent(const TimingTask& task, const TimerInstance& instance, int64_t now) {
    return {
        .event_type = TimingEventType::kInstanceCreated,
        .event_id = instance.id + "/created",
        .task_id = task.id,
        .instance_id = instance.id,
        .schedule_id = task.schedule_id,
        .planned_at = instance.planned_at,
        .trigger_at = instance.planned_at,
        .status = TimingEventStatus::kPending,
        .occurred_at = now,
    };
}

Result<ReminderTrigger> BuildTrigger(const TimingTask& task, const TimerInstance& instance, const ReminderRule& rule,
                                     int64_t now) {
    const auto offset_seconds = static_cast<int64_t>(rule.offset_minutes) * kSecondsPerMinute;
    const auto planned_trigger_at = AddSeconds(instance.planned_at, offset_seconds);
    if (!planned_trigger_at.has_value()) {
        return Result<ReminderTrigger>::Failure(ErrorCode::kInvalidArgument, "提醒触发时间超出范围");
    }
    return Result<ReminderTrigger>::Success({
        .id = TriggerId(instance, rule),
        .reminder_rule_id = rule.id,
        .task_id = task.id,
        .instance_id = instance.id,
        .type = rule.type,
        .planned_trigger_at = *planned_trigger_at,
        .status = ReminderTriggerStatus::kPending,
        .max_snooze_count = rule.max_snooze_count,
        .created_at = now,
        .updated_at = now,
    });
}

TimingEvent BuildTriggerEvent(const TimingTask& task, const ReminderTrigger& trigger, int64_t now) {
    return {
        .event_type = TimingEventType::kReminderTriggered,
        .event_id = trigger.id + "/triggered",
        .task_id = task.id,
        .instance_id = trigger.instance_id,
        .reminder_rule_id = trigger.reminder_rule_id,
        .reminder_trigger_id = trigger.id,
        .schedule_id = task.schedule_id,
        .planned_at = trigger.planned_trigger_at,
        .trigger_at = trigger.planned_trigger_at,
        .status = TimingEventStatus::kPending,
        .occurred_at = now,
    };
}

}  // namespace

Result<AdvanceDueTasksResult> DefaultTimingTaskService::AdvanceDueTasks(int64_t now) {
    if (now < 0) {
        return Result<AdvanceDueTasksResult>::Failure(ErrorCode::kInvalidArgument, "到期推进时间无效");
    }
    if (now == std::numeric_limits<int64_t>::max()) {
        return Result<AdvanceDueTasksResult>::Failure(ErrorCode::kInvalidArgument, "到期推进时间超出范围");
    }

    const auto tasks = store_.ListTasks();
    if (!tasks.ok()) {
        return Result<AdvanceDueTasksResult>::Failure(tasks.status.code, tasks.status.message);
    }

    AdvanceDueTasksResult result{};
    for (const auto& task : *tasks.value) {
        if (task.status != TimingTaskStatus::kActive || task.deleted_at != 0 || task.next_trigger_at == 0 ||
            task.next_trigger_at > now) {
            continue;
        }

        const auto due = PlanOccurrences(task, task.next_trigger_at, now + 1);
        if (!due.ok()) {
            return Result<AdvanceDueTasksResult>::Failure(due.status.code, due.status.message);
        }
        const auto instances = store_.ListInstances(task.id);
        if (!instances.ok()) {
            return Result<AdvanceDueTasksResult>::Failure(instances.status.code, instances.status.message);
        }
        const auto rules = store_.ListRules(task.id);
        if (!rules.ok()) {
            return Result<AdvanceDueTasksResult>::Failure(rules.status.code, rules.status.message);
        }
        const auto triggers = store_.ListTriggers();
        if (!triggers.ok()) {
            return Result<AdvanceDueTasksResult>::Failure(triggers.status.code, triggers.status.message);
        }

        std::unordered_map<int64_t, TimerInstance> existing_instances;
        for (const auto& instance : *instances.value) {
            existing_instances.insert_or_assign(instance.planned_at, instance);
        }
        std::unordered_set<std::string> existing_trigger_ids;
        for (const auto& trigger : *triggers.value) {
            if (trigger.task_id == task.id) {
                existing_trigger_ids.insert(trigger.id);
            }
        }

        TimingTask updated_task = task;
        std::vector<TimerInstance> upsert_instances;
        std::vector<ReminderTrigger> upsert_triggers;
        std::vector<TimingEvent> events;
        int new_instance_count = 0;
        for (const int64_t planned_at : *due.value) {
            TimerInstance instance{};
            const auto existing = existing_instances.find(planned_at);
            if (existing == existing_instances.end()) {
                instance = BuildInstance(task, planned_at, now);
                existing_instances.emplace(planned_at, instance);
                events.push_back(BuildInstanceEvent(task, instance, now));
                ++new_instance_count;
            } else {
                instance = existing->second;
            }

            if (instance.status == TimerInstanceStatus::kSkipped ||
                instance.status == TimerInstanceStatus::kCompleted) {
                continue;
            }
            if (instance.status == TimerInstanceStatus::kPending || instance.status == TimerInstanceStatus::kModified) {
                instance.status = TimerInstanceStatus::kTriggered;
                instance.updated_at = now;
                upsert_instances.push_back(instance);
            }

            for (const auto& rule : *rules.value) {
                if (rule.status != ReminderRuleStatus::kActive) {
                    continue;
                }
                const std::string trigger_id = TriggerId(instance, rule);
                if (existing_trigger_ids.contains(trigger_id)) {
                    continue;
                }
                const auto trigger = BuildTrigger(task, instance, rule, now);
                if (!trigger.ok()) {
                    return Result<AdvanceDueTasksResult>::Failure(trigger.status.code, trigger.status.message);
                }
                existing_trigger_ids.insert(trigger_id);
                upsert_triggers.push_back(*trigger.value);
                events.push_back(BuildTriggerEvent(task, *trigger.value, now));
            }
        }

        const auto next = FindNextOccurrence(task, now);
        if (!next.ok()) {
            return Result<AdvanceDueTasksResult>::Failure(next.status.code, next.status.message);
        }
        updated_task.next_trigger_at = next.value->value_or(0);
        if (upsert_instances.empty() && upsert_triggers.empty() && events.empty() &&
            updated_task.next_trigger_at == task.next_trigger_at) {
            continue;
        }

        const int new_trigger_count = static_cast<int>(upsert_triggers.size());
        const int event_count = static_cast<int>(events.size());

        const Status saved = store_.AdvanceTaskWithFacts({
            .task = updated_task,
            .upsert_instances = std::move(upsert_instances),
            .upsert_triggers = std::move(upsert_triggers),
            .events = std::move(events),
        });
        if (!saved.ok()) {
            return Result<AdvanceDueTasksResult>::Failure(saved.code, saved.message);
        }
        result.materialized_instance_count += new_instance_count;
        result.derived_trigger_count += new_trigger_count;
        result.emitted_event_count += event_count;
    }
    return Result<AdvanceDueTasksResult>::Success(result);
}

}  // namespace voicelife::timing
