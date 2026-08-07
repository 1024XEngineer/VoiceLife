#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/timing/timing_task_service.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::ReminderTriggerStatus;
using voicelife::timing::ReminderType;
using voicelife::timing::TimingClockPort;
using voicelife::timing::TimingEventStatus;
using voicelife::timing::TimingEventType;
using voicelife::timing::TimingIdGeneratorPort;

namespace {

class FixedClock final : public TimingClockPort {
   public:
    int64_t Now() const override { return 1000; }
};

class FixedIds final : public TimingIdGeneratorPort {
   public:
    std::string NextTaskId() override { return "unused-task"; }
    std::string NextReminderRuleId() override { return "unused-rule"; }
};

void AddTriggeredStrong(InMemoryTimingTaskStore& store, int snooze_count = 0) {
    store.AddReminderTrigger({
        .id = "trigger-1",
        .reminder_rule_id = "rule-1",
        .task_id = "task-1",
        .instance_id = "instance-1",
        .type = ReminderType::kStrong,
        .planned_trigger_at = 900,
        .actual_trigger_at = 900,
        .status = ReminderTriggerStatus::kTriggered,
        .snooze_count = snooze_count,
        .max_snooze_count = 2,
        .created_at = 100,
        .updated_at = 900,
    });
}

}  // namespace

int main() {
    FixedClock clock;
    FixedIds ids;

    InMemoryTimingTaskStore success_store;
    AddTriggeredStrong(success_store);
    DefaultTimingTaskService success_service(success_store, clock, ids);
    const auto snoozed = success_service.SnoozeReminderTrigger({
        .reminder_trigger_id = "trigger-1",
        .delay_minutes = 5,
    });
    Check(snoozed.ok(), "triggered 强提醒应允许推迟");
    Check(snoozed.ok() && snoozed.value->status == ReminderTriggerStatus::kSnoozed, "推迟后状态应为 snoozed");
    Check(snoozed.ok() && snoozed.value->snooze_count == 1, "推迟后次数应增加一次");
    Check(snoozed.ok() && snoozed.value->actual_trigger_at == 1300, "推迟后 actual_trigger_at 应为当前时间加延迟");
    Check(snoozed.ok() && snoozed.value->last_action_at == 1000 && snoozed.value->updated_at == 1000,
          "推迟应记录动作和更新时间");
    const auto event = success_store.FindTimingEvent("trigger-1/snooze/1");
    Check(event.ok() && event.value->event_type == TimingEventType::kReminderSnoozed &&
              event.value->status == TimingEventStatus::kSnoozed && event.value->trigger_at == 1300 &&
              event.value->occurred_at == 1000,
          "推迟应原子记录对应的 snoozed 事件事实");
    Check(success_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = 5}).status.code ==
              ErrorCode::kConflict,
          "已 snoozed 的提醒不能重复推迟");

    InMemoryTimingTaskStore invalid_store;
    AddTriggeredStrong(invalid_store);
    DefaultTimingTaskService invalid_service(invalid_store, clock, ids);
    Check(invalid_service.SnoozeReminderTrigger({}).status.code == ErrorCode::kInvalidArgument,
          "缺少 trigger 标识应拒绝");
    Check(invalid_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = 0}).status.code ==
              ErrorCode::kInvalidArgument,
          "零延迟应拒绝");
    Check(
        invalid_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = -1}).status.code ==
            ErrorCode::kInvalidArgument,
        "负延迟应拒绝");

    invalid_store.AddReminderTrigger({
        .id = "weak-trigger",
        .reminder_rule_id = "rule-weak",
        .task_id = "task-1",
        .type = ReminderType::kWeak,
        .status = ReminderTriggerStatus::kTriggered,
        .max_snooze_count = 0,
    });
    Check(invalid_service.SnoozeReminderTrigger({.reminder_trigger_id = "weak-trigger", .delay_minutes = 5})
                  .status.code == ErrorCode::kInvalidArgument,
          "弱提醒不能推迟");
    Check(invalid_service.SnoozeReminderTrigger({.reminder_trigger_id = "missing", .delay_minutes = 5}).status.code ==
              ErrorCode::kNotFound,
          "未知 trigger 应返回 not found");

    invalid_store.AddReminderTrigger({
        .id = "pending-trigger",
        .reminder_rule_id = "rule-pending",
        .task_id = "task-1",
        .type = ReminderType::kStrong,
        .status = ReminderTriggerStatus::kPending,
        .max_snooze_count = 2,
    });
    Check(invalid_service.SnoozeReminderTrigger({.reminder_trigger_id = "pending-trigger", .delay_minutes = 5})
                  .status.code == ErrorCode::kConflict,
          "非 triggered 状态不能推迟");

    InMemoryTimingTaskStore limit_store;
    AddTriggeredStrong(limit_store, 2);
    DefaultTimingTaskService limit_service(limit_store, clock, ids);
    Check(limit_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = 5}).status.code ==
              ErrorCode::kConflict,
          "达到 snooze 次数上限应拒绝");

    InMemoryTimingTaskStore failure_store;
    AddTriggeredStrong(failure_store);
    failure_store.FailNextReminderTriggerUpdate(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    DefaultTimingTaskService failure_service(failure_store, clock, ids);
    Check(failure_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = 5}).status.code ==
              ErrorCode::kUnavailable,
          "Store 写入失败应透传 unavailable");
    const auto retained = failure_store.FindReminderTrigger("trigger-1");
    Check(retained.ok() && retained.value->status == ReminderTriggerStatus::kTriggered &&
              retained.value->snooze_count == 0 && retained.value->actual_trigger_at == 900,
          "Store 写入失败不能增加次数或改变 trigger");
    Check(failure_store.FindTimingEvent("trigger-1/snooze/1").status.code == ErrorCode::kNotFound,
          "Store 写入失败不能残留待投递事件");

    InMemoryTimingTaskStore list_failure_store;
    AddTriggeredStrong(list_failure_store);
    list_failure_store.FailNextTriggerList(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    DefaultTimingTaskService list_failure_service(list_failure_store, clock, ids);
    Check(list_failure_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = 5})
                  .status.code == ErrorCode::kUnavailable,
          "读取 trigger 失败应透传 unavailable");
    return 0;
}
