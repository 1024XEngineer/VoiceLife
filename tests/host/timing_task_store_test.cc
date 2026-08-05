#include "support/test_support.h"
#include "support/timing_fakes.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::ReminderRule;
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

    const ReminderRule first_on_time_strong_rule{
        .id = "rule-on-time-strong-1",
        .task_id = "task-1",
        .type = voicelife::timing::ReminderType::kStrong,
        .offset_minutes = 0,
        .max_snooze_count = 3,
        .snooze_interval_minutes = 5,
    };
    Check(store.UpsertRules("task-1", {first_on_time_strong_rule}).ok(), "首条准点强提醒规则应原子保存成功");

    const ReminderRule second_on_time_strong_rule{
        .id = "rule-on-time-strong-2",
        .task_id = "task-1",
        .type = voicelife::timing::ReminderType::kStrong,
        .offset_minutes = 0,
        .max_snooze_count = 3,
        .snooze_interval_minutes = 5,
    };
    const auto on_time_strong_conflict = store.UpsertRules("task-1", {second_on_time_strong_rule});
    Check(on_time_strong_conflict.code == ErrorCode::kConflict, "Store 必须在原子写入边界拒绝第二条准点强提醒规则");
    const auto rules_after_on_time_strong_conflict = store.ListRules("task-1");
    Check(rules_after_on_time_strong_conflict.ok() && rules_after_on_time_strong_conflict.value->size() == 2,
          "准点强提醒冲突后不能写入第二条规则");
    return 0;
}
