#include "voicelife/timing/timing_task_types.h"

#include "support/test_support.h"

using voicelife::test::Check;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::RecurrenceRule;
using voicelife::timing::TimingTask;
using voicelife::timing::TimingTaskStatus;

template <typename Rule>
concept HasRecurrenceStartAt = requires(Rule rule) { rule.start_at; };

template <typename Rule>
concept HasRecurrenceTimeZone = requires(Rule rule) { rule.time_zone; };

static_assert(!HasRecurrenceStartAt<RecurrenceRule>);
static_assert(!HasRecurrenceTimeZone<RecurrenceRule>);

int main() {
    const TimingTask task{
        .id = "task-1",
        .schedule_id = "schedule-1",
        .start_at = 1785747600,
        .next_trigger_at = 1785747600,
    };

    Check(task.status == TimingTaskStatus::kActive, "新定时任务默认应处于 active 状态");
    Check(task.start_at == task.next_trigger_at, "新任务的首次触发时间应等于周期锚点");
    Check(task.recurrence.frequency == RecurrenceFrequency::kNone, "未提供循环规则时应视为一次性任务");
    return 0;
}
