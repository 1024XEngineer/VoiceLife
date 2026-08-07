#include <array>
#include <cstddef>
#include <string>

#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/timing/timing_task_service.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::DismissReminderTriggerCommand;
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

void AddStrongTrigger(InMemoryTimingTaskStore& store, const std::string& id, ReminderTriggerStatus status) {
    store.AddReminderTrigger({
        .id = id,
        .reminder_rule_id = "rule-" + id,
        .task_id = "task-1",
        .instance_id = "instance-1",
        .type = ReminderType::kStrong,
        .planned_trigger_at = 900,
        .actual_trigger_at = 900,
        .status = status,
        .snooze_count = 1,
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
    AddStrongTrigger(success_store, "triggered", ReminderTriggerStatus::kTriggered);
    AddStrongTrigger(success_store, "snoozed", ReminderTriggerStatus::kSnoozed);
    DefaultTimingTaskService success_service(success_store, clock, ids);

    const auto dismissed = success_service.DismissReminderTrigger({.reminder_trigger_id = "triggered"});
    Check(dismissed.ok() && dismissed.value->status == ReminderTriggerStatus::kDismissed,
          "triggered 强提醒应进入 dismissed");
    Check(dismissed.ok() && dismissed.value->last_action_at == 1000 && dismissed.value->updated_at == 1000,
          "dismiss 应记录动作和更新时间");
    Check(success_service.DismissReminderTrigger({.reminder_trigger_id = "snoozed"}).ok(),
          "snoozed 强提醒也应进入 dismissed");
    Check(success_service.DismissReminderTrigger({.reminder_trigger_id = "triggered"}).status.code ==
              ErrorCode::kConflict,
          "重复 dismiss 应返回 conflict");

    const auto stored = success_store.FindReminderTrigger("triggered");
    Check(stored.ok() && stored.value->status == ReminderTriggerStatus::kDismissed && stored.value->snooze_count == 1 &&
              stored.value->actual_trigger_at == 900,
          "dismiss 成功后 Store 应保留 trigger 其他事实并更新状态");
    const auto event = success_store.FindTimingEvent("triggered/dismiss");
    Check(event.ok() && event.value->event_type == TimingEventType::kReminderDismissed &&
              event.value->status == TimingEventStatus::kDismissed && event.value->trigger_at == 900 &&
              event.value->occurred_at == 1000 && event.value->payload == "提醒内容",
          "dismiss 应原子记录 dismissed 事件事实");

    InMemoryTimingTaskStore invalid_store;
    invalid_store.AddReminderTrigger({
        .id = "weak",
        .type = ReminderType::kWeak,
        .status = ReminderTriggerStatus::kTriggered,
    });
    const std::array<ReminderTriggerStatus, 4> terminal_statuses{
        ReminderTriggerStatus::kDelivered,
        ReminderTriggerStatus::kSkipped,
        ReminderTriggerStatus::kCancelled,
        ReminderTriggerStatus::kFailed,
    };
    for (size_t index = 0; index < terminal_statuses.size(); ++index) {
        AddStrongTrigger(invalid_store, "terminal-" + std::to_string(index), terminal_statuses[index]);
    }
    DefaultTimingTaskService invalid_service(invalid_store, clock, ids);
    Check(invalid_service.DismissReminderTrigger({}).status.code == ErrorCode::kInvalidArgument,
          "缺少 trigger 标识应返回 invalid argument");
    Check(invalid_service.DismissReminderTrigger({.reminder_trigger_id = "weak"}).status.code ==
              ErrorCode::kInvalidArgument,
          "弱提醒不能 dismiss");
    Check(
        invalid_service.DismissReminderTrigger({.reminder_trigger_id = "missing"}).status.code == ErrorCode::kNotFound,
        "未知 trigger 应返回 not found");
    for (size_t index = 0; index < terminal_statuses.size(); ++index) {
        const auto result =
            invalid_service.DismissReminderTrigger({.reminder_trigger_id = "terminal-" + std::to_string(index)});
        Check(result.status.code == ErrorCode::kConflict, "终态 trigger 不能 dismiss");
    }

    InMemoryTimingTaskStore failure_store;
    AddStrongTrigger(failure_store, "failure", ReminderTriggerStatus::kTriggered);
    failure_store.FailNextReminderTriggerUpdate(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    DefaultTimingTaskService failure_service(failure_store, clock, ids);
    Check(failure_service.DismissReminderTrigger({.reminder_trigger_id = "failure"}).status.code ==
              ErrorCode::kUnavailable,
          "Store 写入失败应透传 unavailable");
    const auto retained = failure_store.FindReminderTrigger("failure");
    Check(retained.ok() && retained.value->status == ReminderTriggerStatus::kTriggered &&
              retained.value->updated_at == 900,
          "Store 写入失败不能改变 trigger 原状态");
    Check(failure_store.FindTimingEvent("failure/dismiss").status.code == ErrorCode::kNotFound,
          "Store 写入失败不能残留 dismissed 事件");

    InMemoryTimingTaskStore list_failure_store;
    AddStrongTrigger(list_failure_store, "list-failure", ReminderTriggerStatus::kTriggered);
    list_failure_store.FailNextTriggerList(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    DefaultTimingTaskService list_failure_service(list_failure_store, clock, ids);
    Check(list_failure_service.DismissReminderTrigger({.reminder_trigger_id = "list-failure"}).status.code ==
              ErrorCode::kUnavailable,
          "读取 trigger 失败应透传 unavailable");
    return 0;
}
