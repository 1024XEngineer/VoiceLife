#pragma once

#include "support/test_support.h"
#include "voicelife/timing/timing_task_store.h"

namespace voicelife::test {

inline void RunTimingStoreContract(timing::TimingTaskStorePort& store, const std::string& prefix) {
    using namespace timing;
    const TimingTask task{
        .id = prefix + "-task", .schedule_id = prefix + "-schedule", .next_trigger_at = 100,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.start_at = 100, .time_zone = "Asia/Shanghai"},
        .created_at = 1, .updated_at = 1,
    };
    const ReminderRule rule{
        .id = prefix + "-rule", .task_id = task.id, .type = ReminderType::kStrong,
        .max_snooze_count = 3, .created_at = 1, .updated_at = 1,
    };
    Check(store.RegisterTaskWithRules(task, {rule}).ok(), "store contract: atomic registration");
    Check(store.RegisterTaskWithRules(task, {rule}).code == ErrorCode::kConflict,
          "store contract: duplicate registration conflict");
    Check(store.FindTask(task.id).ok() && store.ListRules(task.id).value->size() == 1,
          "store contract: registered aggregate readable");

    const TimerInstance instance{
        .id = prefix + "-instance", .task_id = task.id, .planned_at = 100,
        .actual_trigger_at = 100, .created_at = 2, .updated_at = 2,
    };
    const ReminderTrigger trigger{
        .id = prefix + "-trigger", .reminder_rule_id = rule.id, .task_id = task.id,
        .instance_id = instance.id, .type = ReminderType::kStrong,
        .planned_trigger_at = 100, .actual_trigger_at = 100,
        .max_snooze_count = 3, .created_at = 2, .updated_at = 2,
    };
    TimingTask advanced = task;
    advanced.next_trigger_at = 0;
    const TimingEvent event{
        .event_type = TimingEventType::kInstanceCreated, .event_id = prefix + "-event",
        .task_id = task.id, .instance_id = instance.id, .schedule_id = task.schedule_id,
        .planned_at = 100, .trigger_at = 100, .occurred_at = 2,
    };
    Check(store.MaterializeOccurrence(instance, {trigger}, advanced, event).ok(),
          "store contract: occurrence transaction");
    Check(store.ListDueTriggers(100).value->size() == 1,
          "store contract: due trigger query");
    Check(store.ListPendingEvents().value->size() == 1,
          "store contract: durable outbox enqueue");
    Check(store.MarkEventPublished(event.event_id).ok() &&
              store.MarkEventPublished(event.event_id).ok() &&
              store.ListPendingEvents().value->empty(),
          "store contract: idempotent outbox acknowledgement");
    const auto disabled = store.DisableRuleAndCancelPendingTriggers(rule.id, 3);
    Check(disabled.ok() && *disabled.value == 1,
          "store contract: rule disable and trigger cancellation are atomic");
}

}  // namespace voicelife::test
