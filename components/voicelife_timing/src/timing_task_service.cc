#include "voicelife/timing/timing_task_service.h"

#include <utility>

#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {
namespace {

bool ValuesInRange(const std::vector<int>& values, int minimum, int maximum) {
    for (const int value : values) {
        if (value < minimum || value > maximum) {
            return false;
        }
    }
    return true;
}

Status ValidateRecurrence(const RecurrenceRule& recurrence) {
    const bool has_weekdays = !recurrence.by_weekdays.empty();
    const bool has_month_days = !recurrence.by_month_days.empty();
    const bool has_months = !recurrence.by_months.empty();

    switch (recurrence.frequency) {
        case RecurrenceFrequency::kNone:
        case RecurrenceFrequency::kDay:
            if (has_weekdays || has_month_days || has_months) {
                return Status::Error(ErrorCode::kInvalidArgument, "当前周期频率不接受筛选字段");
            }
            break;
        case RecurrenceFrequency::kWeek:
            if (has_month_days || has_months || !ValuesInRange(recurrence.by_weekdays, 1, 7)) {
                return Status::Error(ErrorCode::kInvalidArgument, "每周规则的筛选字段或星期值无效");
            }
            break;
        case RecurrenceFrequency::kMonth:
            if (has_weekdays || has_months || !ValuesInRange(recurrence.by_month_days, 1, 31)) {
                return Status::Error(ErrorCode::kInvalidArgument, "每月规则的筛选字段或日期值无效");
            }
            break;
        case RecurrenceFrequency::kYear:
            if (has_weekdays || !ValuesInRange(recurrence.by_month_days, 1, 31) ||
                !ValuesInRange(recurrence.by_months, 1, 12)) {
                return Status::Error(ErrorCode::kInvalidArgument, "每年规则的筛选字段、月份或日期值无效");
            }
            break;
    }
    return Status::Ok();
}

}  // namespace

Result<RegisterTimerTaskResult> DefaultTimingTaskService::RegisterTimerTask(const RegisterTimerTaskCommand& command) {
    const Status recurrence_status = ValidateRecurrence(command.recurrence);
    if (!recurrence_status.ok()) {
        return Result<RegisterTimerTaskResult>::Failure(recurrence_status.code, recurrence_status.message);
    }

    const RegisterTimingTaskCommand policy_command{
        .schedule_id = command.schedule_id,
        .starts_at = command.start_at,
        .time_zone = command.time_zone,
    };

    auto task = TimingPolicy{}.Register(policy_command, ids_.NextTaskId(), clock_.Now());
    if (!task.ok()) {
        return Result<RegisterTimerTaskResult>::Failure(task.status.code, task.status.message);
    }

    task.value->recurrence = command.recurrence;
    task.value->updated_at = task.value->created_at;

    ReminderRule weak_rule{};
    weak_rule.id = ids_.NextReminderRuleId();
    weak_rule.task_id = task.value->id;
    weak_rule.type = ReminderType::kWeak;
    weak_rule.offset_minutes = -10;
    weak_rule.channel = "voice";
    weak_rule.source = "system_default";
    weak_rule.created_at = task.value->created_at;
    weak_rule.updated_at = task.value->created_at;

    ReminderRule strong_rule{};
    strong_rule.id = ids_.NextReminderRuleId();
    strong_rule.task_id = task.value->id;
    strong_rule.type = ReminderType::kStrong;
    strong_rule.max_snooze_count = 3;
    strong_rule.snooze_interval_minutes = 5;
    strong_rule.channel = "voice";
    strong_rule.source = "system_default";
    strong_rule.created_at = task.value->created_at;
    strong_rule.updated_at = task.value->created_at;

    const Status saved = store_.RegisterTaskWithRules(*task.value, {weak_rule, strong_rule});
    if (!saved.ok()) {
        return Result<RegisterTimerTaskResult>::Failure(saved.code, saved.message);
    }

    return Result<RegisterTimerTaskResult>::Success({
        .task_id = std::move(task.value->id),
        .status = task.value->status,
        .next_trigger_at = task.value->next_trigger_at,
    });
}

}  // namespace voicelife::timing
