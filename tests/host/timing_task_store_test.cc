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
        .start_at = 1785747600,
        .next_trigger_at = 1785747600,
    };
    const ReminderRule first_rule{
        .id = "rule-1",
        .task_id = "task-1",
    };
    Check(store.RegisterTaskWithRules(first_task, {first_rule}).ok(), "首个任务和规则应保存成功");

    const TimingTask conflicting_task{
        .id = "task-2",
        .schedule_id = "schedule-2",
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
    return 0;
}
