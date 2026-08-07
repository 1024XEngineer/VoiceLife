#include <array>
#include <string>
#include <utility>

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
using voicelife::timing::SortOrder;
using voicelife::timing::TimingClockPort;
using voicelife::timing::TimingIdGeneratorPort;
using voicelife::timing::TimingTask;
using voicelife::timing::TriggerSortBy;

namespace {

class FixedTimingClock final : public TimingClockPort {
   public:
    int64_t Now() const override { return 1785740000; }
};

class FixedTimingIdGenerator final : public TimingIdGeneratorPort {
   public:
    std::string NextTaskId() override { return "unused-task"; }
    std::string NextReminderRuleId() override { return "unused-rule"; }
};

ReminderTrigger Trigger(std::string id, std::string task_id, std::string instance_id, ReminderType type,
                        ReminderTriggerStatus status, int64_t actual_trigger_at, int64_t created_at) {
    return {
        .id = std::move(id),
        .reminder_rule_id = "rule-" + task_id,
        .task_id = std::move(task_id),
        .instance_id = std::move(instance_id),
        .type = type,
        .planned_trigger_at = actual_trigger_at - 60,
        .actual_trigger_at = actual_trigger_at,
        .status = status,
        .created_at = created_at,
    };
}

}  // namespace

int main() {
    InMemoryTimingTaskStore store;
    FixedTimingClock clock;
    FixedTimingIdGenerator ids;
    DefaultTimingTaskService service(store, clock, ids);

    store.AddTask({.id = "task-a", .schedule_id = "schedule-a"});
    store.AddTask({.id = "task-b", .schedule_id = "schedule-b"});
    store.AddTask({.id = "task-epoch", .schedule_id = "schedule-epoch"});
    store.AddInstance({.id = "instance-a", .task_id = "task-a"});
    store.AddInstance({.id = "instance-empty", .task_id = "task-a"});
    store.AddInstance({.id = "instance-epoch", .task_id = "task-epoch"});

    store.AddReminderTrigger(Trigger("trigger-a-01-pending", "task-a", "instance-a", ReminderType::kWeak,
                                     ReminderTriggerStatus::kPending, 200, 100));
    store.AddReminderTrigger(Trigger("trigger-a-02-triggered", "task-a", "instance-a", ReminderType::kStrong,
                                     ReminderTriggerStatus::kTriggered, 200, 200));
    store.AddReminderTrigger(Trigger("trigger-a-03-delivered", "task-a", "instance-a", ReminderType::kWeak,
                                     ReminderTriggerStatus::kDelivered, 300, 300));
    store.AddReminderTrigger(Trigger("trigger-a-04-skipped", "task-a", "instance-a", ReminderType::kStrong,
                                     ReminderTriggerStatus::kSkipped, 400, 400));
    store.AddReminderTrigger(Trigger("trigger-a-05-failed", "task-a", "instance-a", ReminderType::kWeak,
                                     ReminderTriggerStatus::kFailed, 500, 500));
    store.AddReminderTrigger(Trigger("trigger-a-06-snoozed", "task-a", "instance-a", ReminderType::kStrong,
                                     ReminderTriggerStatus::kSnoozed, 600, 600));
    store.AddReminderTrigger(Trigger("trigger-a-07-dismissed", "task-a", "instance-a", ReminderType::kStrong,
                                     ReminderTriggerStatus::kDismissed, 700, 700));
    store.AddReminderTrigger(Trigger("trigger-a-08-cancelled", "task-a", "instance-a", ReminderType::kWeak,
                                     ReminderTriggerStatus::kCancelled, 800, 800));
    store.AddReminderTrigger(Trigger("trigger-b-01", "task-b", "instance-b", ReminderType::kStrong,
                                     ReminderTriggerStatus::kDelivered, 250, 900));
    store.AddReminderTrigger(Trigger("trigger-epoch", "task-epoch", "instance-epoch", ReminderType::kWeak,
                                     ReminderTriggerStatus::kPending, 0, 0));

    const auto epoch_range = service.ListReminderTriggers({.range_start = 0, .range_end = 1});
    Check(epoch_range.ok() && epoch_range.value->total == 1 &&
              epoch_range.value->reminder_triggers.front().id == "trigger-epoch",
          "Unix epoch 0 应作为合法时间范围边界参与查询");

    const auto range = service.ListReminderTriggers({.range_start = 200, .range_end = 800});
    Check(range.ok() && range.value->total == 8, "时间范围自身应作为合法查询条件并遵守左闭右开语义");

    const auto filtered = service.ListReminderTriggers({
        .type = ReminderType::kStrong,
        .status = ReminderTriggerStatus::kTriggered,
    });
    Check(filtered.ok() && filtered.value->total == 1 &&
              filtered.value->reminder_triggers.front().id == "trigger-a-02-triggered",
          "weak/strong 类型和状态过滤应可组合使用");

    for (const auto& [status, id] : std::array{
             std::pair{ReminderTriggerStatus::kPending, "trigger-a-01-pending"},
             std::pair{ReminderTriggerStatus::kTriggered, "trigger-a-02-triggered"},
             std::pair{ReminderTriggerStatus::kDelivered, "trigger-a-03-delivered"},
             std::pair{ReminderTriggerStatus::kSkipped, "trigger-a-04-skipped"},
             std::pair{ReminderTriggerStatus::kFailed, "trigger-a-05-failed"},
             std::pair{ReminderTriggerStatus::kSnoozed, "trigger-a-06-snoozed"},
             std::pair{ReminderTriggerStatus::kDismissed, "trigger-a-07-dismissed"},
             std::pair{ReminderTriggerStatus::kCancelled, "trigger-a-08-cancelled"},
         }) {
        const auto by_status = service.ListReminderTriggers({.task_id = "task-a", .status = status});
        Check(by_status.ok() && by_status.value->total == 1 && by_status.value->reminder_triggers.front().id == id,
              "每种提醒触发状态都应能被独立过滤");
    }

    const auto by_schedule = service.ListReminderTriggers({.schedule_id = "schedule-a"});
    Check(by_schedule.ok() && by_schedule.value->total == 8, "日程过滤应仅返回关联任务的触发记录");
    const auto by_instance = service.ListReminderTriggers({.instance_id = "instance-a"});
    Check(by_instance.ok() && by_instance.value->total == 8, "实例过滤应仅返回该 occurrence 的触发记录");
    const auto empty = service.ListReminderTriggers({.task_id = "task-b", .type = ReminderType::kWeak});
    Check(empty.ok() && empty.value->total == 0 && empty.value->reminder_triggers.empty(),
          "存在的资源没有匹配触发时应返回稳定空页");

    const auto second_page = service.ListReminderTriggers({.task_id = "task-a", .page = 2, .page_size = 1});
    Check(second_page.ok() && second_page.value->total == 8 && second_page.value->has_more &&
              second_page.value->reminder_triggers.front().id == "trigger-a-02-triggered",
          "默认排序相同时间时应按标识稳定排序并正确分页");
    const auto latest_created = service.ListReminderTriggers({
        .task_id = "task-a",
        .page_size = 2,
        .sort_by = TriggerSortBy::kCreatedAt,
        .sort_order = SortOrder::kDescending,
    });
    Check(latest_created.ok() && latest_created.value->has_more &&
              latest_created.value->reminder_triggers.front().id == "trigger-a-08-cancelled",
          "created_at 倒序应返回最新触发记录并标记后续页");

    Check(service.ListReminderTriggers({.task_id = "missing-task"}).status.code == ErrorCode::kNotFound,
          "不存在的任务应返回明确错误");
    Check(service.ListReminderTriggers({.instance_id = "missing-instance"}).status.code == ErrorCode::kNotFound,
          "不存在的实例应返回明确错误");
    Check(service.ListReminderTriggers({.schedule_id = "missing-schedule"}).status.code == ErrorCode::kNotFound,
          "Timing 中不存在的日程关联应返回明确错误");

    Check(service.ListReminderTriggers({.range_start = 200}).status.code == ErrorCode::kInvalidArgument,
          "时间范围必须同时提供起点和终点");
    Check(
        service.ListReminderTriggers({.range_start = 200, .range_end = 200}).status.code == ErrorCode::kInvalidArgument,
        "时间范围必须满足左闭右开区间的起点小于终点");
    Check(service.ListReminderTriggers({.task_id = "task-a", .page = 0}).status.code == ErrorCode::kInvalidArgument,
          "页码必须从一开始");
    Check(service.ListReminderTriggers({.task_id = "task-a", .page_size = 101}).status.code ==
              ErrorCode::kInvalidArgument,
          "每页数量不能超过接口约定的上限");

    store.FailNextTriggerList(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    Check(service.ListReminderTriggers({.task_id = "task-a"}).status.code == ErrorCode::kUnavailable,
          "触发记录读取失败应透传 Store 错误");
    return 0;
}
