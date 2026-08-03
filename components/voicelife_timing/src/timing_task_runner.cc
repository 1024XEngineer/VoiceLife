#include "voicelife/timing/timing_task_runner.h"

#include <algorithm>

#include "voicelife/timing/recurrence_policy.h"

namespace voicelife::timing {
namespace {

int64_t EffectiveStart(const TimerInstance& instance) {
    return instance.override_fields.start_at.value_or(instance.planned_at);
}

}  // namespace

Status TimingTaskRunner::PollDue() {
    const int64_t now = clock_.Now();
    RecurrencePolicy recurrence;
    auto tasks = store_.ListTasks();
    if (!tasks.ok()) return tasks.status;

    for (auto task : *tasks.value) {
        if (task.status != TimingTaskStatus::kActive || task.next_trigger_at <= 0) continue;
        if (task.effective_until > 0 && task.next_trigger_at >= task.effective_until) {
            task.status = TimingTaskStatus::kTerminated;
            task.next_trigger_at = 0;
            task.updated_at = now;
            const Status stopped = store_.UpdateTask(task);
            if (!stopped.ok()) return stopped;
            continue;
        }
        auto rules = store_.ListRules(task.id);
        if (!rules.ok()) return rules.status;
        int earliest_offset = 0;
        bool has_rule = false;
        for (const auto& rule : *rules.value) {
            if (rule.status != ReminderRuleStatus::kActive) continue;
            earliest_offset = has_rule ? std::min(earliest_offset, rule.offset_minutes) : rule.offset_minutes;
            has_rule = true;
        }
        if (task.next_trigger_at + earliest_offset * 60 > now) continue;

        auto existing = store_.FindInstanceByOccurrence(task.id, task.next_trigger_at);
        if (!existing.ok()) return existing.status;
        TimerInstance instance = existing.value->value_or(TimerInstance{
            .id = ids_.Next("instance"), .task_id = task.id, .planned_at = task.next_trigger_at,
            .actual_trigger_at = task.next_trigger_at, .status = TimerInstanceStatus::kPending,
            .created_at = now, .updated_at = now,
        });
        const int64_t actual_start = EffectiveStart(instance);
        std::vector<ReminderTrigger> triggers;
        for (const auto& rule : *rules.value) {
            if (rule.status != ReminderRuleStatus::kActive) continue;
            triggers.push_back({
                .id = ids_.Next("trigger"), .reminder_rule_id = rule.id, .task_id = task.id,
                .instance_id = instance.id, .type = rule.type,
                .planned_trigger_at = instance.planned_at + rule.offset_minutes * 60,
                .actual_trigger_at = actual_start + rule.offset_minutes * 60,
                .status = ReminderTriggerStatus::kPending, .max_snooze_count = rule.max_snooze_count,
                .created_at = now, .updated_at = now,
            });
        }
        auto next = recurrence.NextAfter(task.recurrence, task.next_trigger_at);
        if (!next.ok()) return next.status;
        TimingTask advanced = task;
        advanced.next_trigger_at = *next.value;
        if (advanced.pending_recurrence &&
            (advanced.next_trigger_at == 0 ||
             advanced.next_trigger_at >= advanced.pending_effective_from)) {
            advanced.recurrence = *advanced.pending_recurrence;
            advanced.next_trigger_at = advanced.pending_recurrence->start_at;
            advanced.pending_recurrence.reset();
            advanced.pending_effective_from = 0;
        }
        if (advanced.next_trigger_at == 0 ||
            (advanced.effective_until > 0 && advanced.next_trigger_at >= advanced.effective_until)) {
            advanced.status = TimingTaskStatus::kTerminated;
            advanced.next_trigger_at = 0;
        }
        advanced.updated_at = now;
        const TimingEvent event{
            .event_type = TimingEventType::kInstanceCreated, .event_id = ids_.Next("event"),
            .task_id = task.id, .instance_id = instance.id, .schedule_id = task.schedule_id,
            .planned_at = instance.planned_at, .trigger_at = now,
            .status = TimingEventStatus::kPending, .occurred_at = now,
        };
        const Status saved = store_.MaterializeOccurrence(instance, triggers, advanced, event);
        if (!saved.ok() && saved.code != ErrorCode::kConflict) return saved;
    }

    auto due = store_.ListDueTriggers(now);
    if (!due.ok()) return due.status;
    for (auto trigger : *due.value) {
        trigger.last_action_at = now;
        trigger.updated_at = now;
        TimingEventStatus event_status = TimingEventStatus::kTriggered;
        if (trigger.type == ReminderType::kWeak) {
            trigger.status = ReminderTriggerStatus::kDelivered;
            trigger.delivered_at = now;
            event_status = TimingEventStatus::kDelivered;
        } else {
            trigger.status = ReminderTriggerStatus::kTriggered;
        }
        auto task = store_.FindTask(trigger.task_id);
        if (!task.ok()) return task.status;
        const TimingEvent event{
            .event_type = TimingEventType::kReminderTriggered, .event_id = ids_.Next("event"),
            .task_id = trigger.task_id, .instance_id = trigger.instance_id,
            .reminder_rule_id = trigger.reminder_rule_id, .reminder_trigger_id = trigger.id,
            .schedule_id = task.value->schedule_id, .planned_at = trigger.planned_trigger_at,
            .trigger_at = trigger.actual_trigger_at, .status = event_status, .occurred_at = now,
        };
        const Status updated = store_.UpdateTriggerWithEvent(trigger, event);
        if (!updated.ok()) return updated;
    }

    auto pending = store_.ListPendingEvents();
    if (!pending.ok()) return pending.status;
    for (const auto& event : *pending.value) {
        const Status published = events_.Publish(event);
        if (!published.ok()) return published;
        const Status marked = store_.MarkEventPublished(event.event_id);
        if (!marked.ok()) return marked;
    }
    return Status::Ok();
}

}  // namespace voicelife::timing
