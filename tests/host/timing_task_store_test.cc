#include "support/test_support.h"
#include "support/timing_fakes.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::ReminderRule;
using voicelife::timing::TimerInstance;
using voicelife::timing::TimingTask;
using voicelife::timing::TimingTaskStatus;

int main() {
    InMemoryTimingTaskStore store;

    const TimingTask first_task{
        .id = "task-1",
        .schedule_id = "schedule-1",
        .request_id = "request-1",
        .start_at = 1785747600,
        .next_trigger_at = 1785747600,
    };
    const ReminderRule first_rule{
        .id = "rule-1",
        .task_id = "task-1",
    };
    Check(store.RegisterTaskWithRules(first_task, {first_rule}).ok(), "首个任务和规则应保存成功");
    const auto request_match = store.FindTaskByRequestId("request-1");
    Check(request_match.ok() && request_match.value->id == "task-1", "Store 应能按 request_id 回读任务");

    const TimingTask conflicting_task{
        .id = "task-2",
        .schedule_id = "schedule-1",
        .request_id = "request-2",
        .start_at = 1785834000,
        .next_trigger_at = 1785834000,
    };
    const ReminderRule conflicting_rule{
        .id = "rule-1",
        .task_id = "task-2",
    };
    const auto conflict = store.RegisterTaskWithRules(conflicting_task, {conflicting_rule});
    Check(conflict.code == ErrorCode::kConflict, "规则标识冲突应拒绝整批写入");

    const auto task = store.FindTask("task-2");
    Check(task.status.code == ErrorCode::kNotFound, "规则冲突后任务不应残留");
    const auto rules = store.ListRules("task-2");
    Check(rules.ok() && rules.value->empty(), "规则冲突后规则不应残留");

    const TimingTask second_task{
        .id = "task-2",
        .schedule_id = "schedule-2",
        .request_id = "request-2",
        .start_at = 1785834000,
        .next_trigger_at = 1785834000,
    };
    const ReminderRule second_rule{
        .id = "rule-2",
        .task_id = "task-2",
    };
    Check(store.RegisterTaskWithRules(second_task, {second_rule}).ok(), "第二个任务应保存成功");
    const TimerInstance first_instance{
        .id = "instance-1",
        .task_id = "task-1",
        .planned_at = 1785747600,
    };
    Check(store.UpdateTaskWithInstances({.task = first_task, .upsert_instances = {first_instance}}).ok(),
          "首个任务应能保存实例");

    TimingTask terminated_second_task = second_task;
    terminated_second_task.status = TimingTaskStatus::kTerminated;
    const TimerInstance conflicting_instance{
        .id = "instance-1",
        .task_id = "task-2",
        .planned_at = 1785834000,
    };
    const auto instance_conflict =
        store.UpdateTaskWithInstances({.task = terminated_second_task, .upsert_instances = {conflicting_instance}});
    Check(instance_conflict.code == ErrorCode::kConflict, "实例不能改写到另一任务");
    const auto second_task_after_conflict = store.FindTask("task-2");
    Check(second_task_after_conflict.ok() && second_task_after_conflict.value->status == TimingTaskStatus::kActive,
          "实例归属冲突后任务字段不应半更新");
    return 0;
}
