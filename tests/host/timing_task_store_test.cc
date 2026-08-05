#include "support/test_support.h"
#include "support/timing_fakes.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::ReminderRule;
using voicelife::timing::TimerInstance;
using voicelife::timing::TimerInstanceStatus;
using voicelife::timing::TimingTask;

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

    auto updated_task = first_task;
    updated_task.start_at = 1785920400;
    updated_task.next_trigger_at = 1785920400;
    updated_task.updated_at = 1785741000;
    const TimerInstance modified_instance{
        .id = "instance-1",
        .task_id = "task-1",
        .planned_at = 1785747600,
        .status = TimerInstanceStatus::kModified,
        .override_fields = {.start_at = 1785920400},
        .updated_at = 1785741000,
    };
    Check(store
              .UpdateTaskWithInstances({
                  .task = updated_task,
                  .upsert_instances = {modified_instance},
              })
              .ok(),
          "任务字段和实例覆盖应能原子更新");
    const auto stored_update = store.FindTask("task-1");
    Check(stored_update.ok() && stored_update.value->next_trigger_at == 1785920400, "原子更新后应能读回新触发时间");
    const auto stored_instances = store.ListInstances("task-1");
    Check(stored_instances.ok() && stored_instances.value->size() == 1, "原子更新后应能读回实例覆盖");
    Check(stored_instances.ok() && stored_instances.value->front().override_fields.start_at.has_value() &&
              *stored_instances.value->front().override_fields.start_at == 1785920400,
          "实例覆盖应保存新的开始时间");

    auto rejected_task = updated_task;
    rejected_task.next_trigger_at = 1786006800;
    const TimerInstance wrong_owner_instance{
        .id = "instance-wrong-owner",
        .task_id = "task-other",
        .planned_at = 1786006800,
    };
    const auto rejected = store.UpdateTaskWithInstances({
        .task = rejected_task,
        .upsert_instances = {wrong_owner_instance},
    });
    Check(rejected.code == ErrorCode::kConflict, "实例归属错误应拒绝整批更新");
    const auto after_rejected_task = store.FindTask("task-1");
    Check(after_rejected_task.ok() && after_rejected_task.value->next_trigger_at == 1785920400,
          "实例归属错误后任务不应半更新");
    const auto after_rejected_instances = store.ListInstances("task-1");
    Check(after_rejected_instances.ok() && after_rejected_instances.value->size() == 1, "实例归属错误后不应新增覆盖");

    store.FailNextUpdate(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    rejected_task.next_trigger_at = 1786093200;
    const auto unavailable = store.UpdateTaskWithInstances({
        .task = rejected_task,
        .upsert_instances = {},
    });
    Check(unavailable.code == ErrorCode::kUnavailable, "注入的 Store 失败应直接返回");
    const auto after_unavailable_task = store.FindTask("task-1");
    Check(after_unavailable_task.ok() && after_unavailable_task.value->next_trigger_at == 1785920400,
          "Store 失败后任务不应半更新");
    return 0;
}
