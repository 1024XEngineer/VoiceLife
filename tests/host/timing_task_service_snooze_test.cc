#include <limits>

#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/timing/timing_task_service.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::ReminderTrigger;
using voicelife::timing::ReminderTriggerStatus;
using voicelife::timing::ReminderType;
using voicelife::timing::TimingClockPort;
using voicelife::timing::TimingEvent;
using voicelife::timing::TimingEventStatus;
using voicelife::timing::TimingEventType;
using voicelife::timing::TimingIdGeneratorPort;

namespace {

class FixedClock final : public TimingClockPort {
   public:
    explicit FixedClock(int64_t now = 1000) : now_(now) {}

    int64_t Now() const override { return now_; }

   private:
    int64_t now_;
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
        .payload = "提醒内容",
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
    const auto stored_snoozed = success_store.FindReminderTrigger("trigger-1");
    Check(stored_snoozed.ok() && stored_snoozed.value->status == ReminderTriggerStatus::kSnoozed &&
              stored_snoozed.value->snooze_count == 1 && stored_snoozed.value->actual_trigger_at == 1300 &&
              stored_snoozed.value->last_action_at == 1000 && stored_snoozed.value->updated_at == 1000,
          "推迟成功后 Store 应保存完整的 trigger 状态");
    const auto event = success_store.FindTimingEvent("trigger-1/snooze/1");
    Check(event.ok() && event.value->event_type == TimingEventType::kReminderSnoozed &&
              event.value->status == TimingEventStatus::kSnoozed && event.value->trigger_at == 1300 &&
              event.value->occurred_at == 1000 && event.value->payload == "提醒内容",
          "推迟应原子记录对应的 snoozed 事件事实");
    Check(success_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = 5}).status.code ==
              ErrorCode::kConflict,
          "已 snoozed 的提醒不能重复推迟");

    InMemoryTimingTaskStore stale_write_store;
    AddTriggeredStrong(stale_write_store);
    const auto stale_snapshot = stale_write_store.FindReminderTrigger("trigger-1");
    Check(stale_snapshot.ok(), "测试应能读取写入前快照");
    DefaultTimingTaskService stale_write_service(stale_write_store, clock, ids);
    Check(stale_write_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = 5}).ok(),
          "首个 snooze 写入应成功");
    ReminderTrigger stale_trigger = *stale_snapshot.value;
    stale_trigger.status = ReminderTriggerStatus::kSnoozed;
    stale_trigger.snooze_count = 1;
    stale_trigger.actual_trigger_at = 1300;
    stale_trigger.last_action_at = 1000;
    stale_trigger.updated_at = 1000;
    const auto stale_write = stale_write_store.UpdateReminderTriggerWithEvent({
        .trigger = stale_trigger,
        .event =
            TimingEvent{
                .event_type = TimingEventType::kReminderSnoozed,
                .event_id = "trigger-1/stale-snooze",
                .task_id = "task-1",
                .instance_id = "instance-1",
                .reminder_rule_id = "rule-1",
                .reminder_trigger_id = "trigger-1",
                .planned_at = 900,
                .trigger_at = 1300,
                .status = TimingEventStatus::kSnoozed,
                .occurred_at = 1000,
            },
        .expected_status = stale_snapshot.value->status,
        .expected_snooze_count = stale_snapshot.value->snooze_count,
        .expected_updated_at = stale_snapshot.value->updated_at,
    });
    Check(stale_write.code == ErrorCode::kConflict, "过期快照不能覆盖已提交的 snooze 状态");
    Check(stale_write_store.FindTimingEvent("trigger-1/stale-snooze").status.code == ErrorCode::kNotFound,
          "过期写入不能产生第二个 snooze 事件");

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

    FixedClock exact_limit_clock(std::numeric_limits<int64_t>::max() - 60);
    InMemoryTimingTaskStore exact_limit_store;
    AddTriggeredStrong(exact_limit_store);
    DefaultTimingTaskService exact_limit_service(exact_limit_store, exact_limit_clock, ids);
    const auto exact_limit =
        exact_limit_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = 1});
    Check(exact_limit.ok() && exact_limit.value->actual_trigger_at == std::numeric_limits<int64_t>::max(),
          "时间恰好到 int64 上限时应允许推迟");

    FixedClock overflow_clock(std::numeric_limits<int64_t>::max() - 59);
    InMemoryTimingTaskStore overflow_store;
    AddTriggeredStrong(overflow_store);
    DefaultTimingTaskService overflow_service(overflow_store, overflow_clock, ids);
    Check(
        overflow_service.SnoozeReminderTrigger({.reminder_trigger_id = "trigger-1", .delay_minutes = 1}).status.code ==
            ErrorCode::kInvalidArgument,
        "推迟时间溢出时应返回参数错误");
    return 0;
}
