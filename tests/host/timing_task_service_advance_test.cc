#include <cstdint>

#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/timing/timing_task_service.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::ReminderRuleStatus;
using voicelife::timing::ReminderTriggerStatus;
using voicelife::timing::ReminderType;
using voicelife::timing::TimingClockPort;
using voicelife::timing::TimingEventStatus;
using voicelife::timing::TimingEventType;
using voicelife::timing::TimingIdGeneratorPort;
using voicelife::timing::TimingTaskStatus;

namespace {

class FixedTimingClock final : public TimingClockPort {
   public:
    int64_t Now() const override { return 100; }
};

class FixedTimingIdGenerator final : public TimingIdGeneratorPort {
   public:
    std::string NextTaskId() override { return "task-"; }
    std::string NextReminderRuleId() override { return "rule-"; }
};

}  // namespace

int main() {
    InMemoryTimingTaskStore store;
    store.AddTask({
        .id = "advance-task",
        .schedule_id = "advance-schedule",
        .start_at = 100,
        .next_trigger_at = 100,
        .time_zone = "+08:00",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
        .status = TimingTaskStatus::kActive,
    });
    FixedTimingClock clock;
    FixedTimingIdGenerator ids;
    DefaultTimingTaskService service(store, clock, ids);

    const auto advanced = service.AdvanceDueTasks(100);
    Check(advanced.ok(), "到期任务应能物化 occurrence");

    const auto instances = store.ListInstances("advance-task");
    Check(instances.ok() && instances.value->size() == 1 && instances.value->front().planned_at == 100,
          "到期 occurrence 应持久化为唯一 TimerInstance");
    const auto task = store.FindTask("advance-task");
    Check(task.ok() && task.value->next_trigger_at > 100, "物化后应推进下一次触发时间");

    const auto replayed = service.AdvanceDueTasks(100);
    Check(replayed.ok() && replayed.value->materialized_instance_count == 0, "相同 now 重复推进不应重复物化实例");
    const auto replayed_instances = store.ListInstances("advance-task");
    Check(replayed_instances.ok() && replayed_instances.value->size() == 1, "重复推进不应增加第二个 TimerInstance");

    InMemoryTimingTaskStore failure_store;
    failure_store.AddTask({
        .id = "advance-failure",
        .schedule_id = "advance-failure-schedule",
        .start_at = 100,
        .next_trigger_at = 100,
        .time_zone = "+08:00",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
        .status = TimingTaskStatus::kActive,
    });
    failure_store.FailNextUpdate(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    DefaultTimingTaskService failure_service(failure_store, clock, ids);
    const auto failed = failure_service.AdvanceDueTasks(100);
    Check(failed.status.code == ErrorCode::kUnavailable, "Store 失败应透传到期推进错误");
    Check(failure_store.ListInstances("advance-failure").value->empty(), "复合写入失败时不应留下半个 TimerInstance");
    Check(failure_store.FindTask("advance-failure").value->next_trigger_at == 100,
          "复合写入失败时不应推进 next_trigger_at");

    InMemoryTimingTaskStore catch_up_store;
    catch_up_store.AddTask({
        .id = "advance-catch-up",
        .schedule_id = "advance-catch-up-schedule",
        .start_at = 100,
        .next_trigger_at = 100,
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
        .status = TimingTaskStatus::kActive,
    });
    DefaultTimingTaskService catch_up_service(catch_up_store, clock, ids);
    const auto catch_up = catch_up_service.AdvanceDueTasks(100 + 2 * 24 * 60 * 60);
    Check(catch_up.ok() && catch_up.value->materialized_instance_count == 3,
          "落后多次轮询时应一次补齐所有到期 occurrence");
    Check(catch_up_store.FindTask("advance-catch-up").value->next_trigger_at > 100 + 2 * 24 * 60 * 60,
          "补齐到期 occurrence 后应推进到未来 occurrence");

    InMemoryTimingTaskStore trigger_store;
    trigger_store.AddTask({
        .id = "advance-trigger",
        .schedule_id = "advance-trigger-schedule",
        .start_at = 100,
        .next_trigger_at = 100,
        .time_zone = "+08:00",
        .recurrence = {.frequency = RecurrenceFrequency::kNone},
        .status = TimingTaskStatus::kActive,
    });
    trigger_store.AddReminderRule({
        .id = "weak-rule",
        .task_id = "advance-trigger",
        .type = ReminderType::kWeak,
        .offset_minutes = -10,
        .status = ReminderRuleStatus::kActive,
    });
    trigger_store.AddReminderRule({
        .id = "strong-rule",
        .task_id = "advance-trigger",
        .type = ReminderType::kStrong,
        .offset_minutes = 0,
        .max_snooze_count = 2,
        .snooze_interval_minutes = 5,
        .status = ReminderRuleStatus::kActive,
    });
    trigger_store.AddReminderRule({
        .id = "disabled-rule",
        .task_id = "advance-trigger",
        .type = ReminderType::kWeak,
        .offset_minutes = -20,
        .status = ReminderRuleStatus::kDisabled,
    });
    DefaultTimingTaskService trigger_service(trigger_store, clock, ids);
    const auto trigger_advanced = trigger_service.AdvanceDueTasks(100);
    Check(trigger_advanced.ok() && trigger_advanced.value->materialized_instance_count == 1 &&
              trigger_advanced.value->derived_trigger_count == 2 && trigger_advanced.value->emitted_event_count == 3,
          "到期实例应一次性派生 active 规则的提醒触发和事件");
    const auto trigger_list = trigger_store.ListTriggers();
    Check(trigger_list.ok() && trigger_list.value->size() == 2, "disabled reminder rule 不应派生触发");
    const auto trigger_instances = trigger_store.ListInstances("advance-trigger");
    Check(trigger_instances.ok() && trigger_instances.value->size() == 1 &&
              trigger_instances.value->front().status == voicelife::timing::TimerInstanceStatus::kTriggered,
          "派生提醒事实时应推进到期实例状态");
    const auto strong_trigger = trigger_store.FindReminderTrigger("advance-trigger@100/strong-rule");
    Check(strong_trigger.ok() && strong_trigger.value->status == ReminderTriggerStatus::kPending &&
              strong_trigger.value->planned_trigger_at == 100 && strong_trigger.value->max_snooze_count == 2,
          "强提醒触发应保留规则字段并等待投递");
    const auto instance_event = trigger_store.FindTimingEvent("advance-trigger@100/created");
    Check(instance_event.ok() && instance_event.value->event_type == TimingEventType::kInstanceCreated &&
              instance_event.value->status == TimingEventStatus::kPending,
          "实例创建事件应与实例同一原子提交");
    const auto reminder_event = trigger_store.FindTimingEvent("advance-trigger@100/strong-rule/triggered");
    Check(reminder_event.ok() && reminder_event.value->event_type == TimingEventType::kReminderTriggered &&
              reminder_event.value->reminder_trigger_id == "advance-trigger@100/strong-rule" &&
              reminder_event.value->status == TimingEventStatus::kPending,
          "提醒触发事件应与 ReminderTrigger 同一原子提交");

    InMemoryTimingTaskStore trigger_failure_store;
    trigger_failure_store.AddTask({
        .id = "advance-trigger-failure",
        .schedule_id = "advance-trigger-failure-schedule",
        .start_at = 100,
        .next_trigger_at = 100,
        .recurrence = {.frequency = RecurrenceFrequency::kNone},
        .status = TimingTaskStatus::kActive,
    });
    trigger_failure_store.AddReminderRule({
        .id = "trigger-failure-rule",
        .task_id = "advance-trigger-failure",
        .type = ReminderType::kStrong,
        .offset_minutes = 0,
        .max_snooze_count = 1,
        .snooze_interval_minutes = 5,
        .status = ReminderRuleStatus::kActive,
    });
    trigger_failure_store.FailNextUpdate(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    DefaultTimingTaskService trigger_failure_service(trigger_failure_store, clock, ids);
    const auto trigger_failed = trigger_failure_service.AdvanceDueTasks(100);
    Check(trigger_failed.status.code == ErrorCode::kUnavailable, "提醒复合写入失败应透传 Store 错误");
    Check(trigger_failure_store.ListInstances("advance-trigger-failure").value->empty() &&
              trigger_failure_store.ListTriggers().value->empty() &&
              trigger_failure_store.FindTimingEvent("advance-trigger-failure@100/created").status.code ==
                  ErrorCode::kNotFound,
          "提醒复合写入失败时实例、触发和事件都不应落库");

    InMemoryTimingTaskStore cancelled_store;
    cancelled_store.AddTask({
        .id = "cancelled-task",
        .schedule_id = "cancelled-schedule",
        .start_at = 100,
        .next_trigger_at = 100,
        .status = TimingTaskStatus::kTerminated,
    });
    cancelled_store.AddTask({
        .id = "skipped-task",
        .schedule_id = "skipped-schedule",
        .start_at = 100,
        .next_trigger_at = 100,
        .recurrence = {.frequency = RecurrenceFrequency::kNone},
        .status = TimingTaskStatus::kActive,
    });
    cancelled_store.AddInstance({
        .id = "skipped-task@100",
        .task_id = "skipped-task",
        .planned_at = 100,
        .status = voicelife::timing::TimerInstanceStatus::kSkipped,
    });
    DefaultTimingTaskService cancelled_service(cancelled_store, clock, ids);
    const auto cancelled_advanced = cancelled_service.AdvanceDueTasks(100);
    Check(cancelled_advanced.ok() && cancelled_advanced.value->materialized_instance_count == 0,
          "终止任务和已跳过 occurrence 不应重新物化");
    Check(cancelled_store.ListInstances("cancelled-task").value->empty() &&
              cancelled_store.ListInstances("skipped-task").value->size() == 1,
          "取消和跳过事实应保持不变");

    return 0;
}
