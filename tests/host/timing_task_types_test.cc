#include "voicelife/timing/timing_task_types.h"

#include "support/test_support.h"

using voicelife::test::Check;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::TimingTask;
using voicelife::timing::TimingTaskStatus;

int main() {
    const TimingTask task{
        .id = "task-1",
        .schedule_id = "schedule-1",
        .next_trigger_at = 1785747600,
    };

    Check(task.status == TimingTaskStatus::kActive, "新定时任务默认应处于 active 状态");
    Check(task.recurrence.frequency == RecurrenceFrequency::kNone, "未提供循环规则时应视为一次性任务");
    return 0;
}
