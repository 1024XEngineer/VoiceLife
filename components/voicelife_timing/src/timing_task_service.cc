#include "voicelife/timing/timing_task_service.h"

#include <utility>

#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {
namespace {

template <typename T>
Result<T> NotImplemented(const char* operation) {
    return Result<T>::Failure(ErrorCode::kUnavailable, std::string(operation) + " 尚未实现");
}

}  // namespace

Result<RegisterTimerTaskResult> DefaultTimingTaskService::RegisterTimerTask(const RegisterTimerTaskCommand& command) {
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
    if (task.value->recurrence.start_at == 0) {
        task.value->recurrence.start_at = command.start_at;
    }
    task.value->recurrence.time_zone = command.time_zone;
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

Result<UpdateTimerTaskResult> DefaultTimingTaskService::UpdateTimerTask(const UpdateTimerTaskCommand&) {
    return NotImplemented<UpdateTimerTaskResult>("UpdateTimerTask");
}

Result<CancelTimerTaskResult> DefaultTimingTaskService::CancelTimerTask(const CancelTimerTaskCommand&) {
    return NotImplemented<CancelTimerTaskResult>("CancelTimerTask");
}

Result<UpsertReminderRulesResult> DefaultTimingTaskService::UpsertReminderRules(const UpsertReminderRulesCommand&) {
    return NotImplemented<UpsertReminderRulesResult>("UpsertReminderRules");
}

Result<DeleteReminderRuleResult> DefaultTimingTaskService::DeleteReminderRule(const DeleteReminderRuleCommand&) {
    return NotImplemented<DeleteReminderRuleResult>("DeleteReminderRule");
}

Result<CalendarView> DefaultTimingTaskService::ListCalendarView(const CalendarViewQuery&) {
    return NotImplemented<CalendarView>("ListCalendarView");
}

Result<ReminderTriggerPage> DefaultTimingTaskService::ListReminderTriggers(const ReminderTriggerQuery&) {
    return NotImplemented<ReminderTriggerPage>("ListReminderTriggers");
}

Result<ReminderTrigger> DefaultTimingTaskService::SnoozeReminderTrigger(const SnoozeReminderTriggerCommand&) {
    return NotImplemented<ReminderTrigger>("SnoozeReminderTrigger");
}

Result<ReminderTrigger> DefaultTimingTaskService::DismissReminderTrigger(const DismissReminderTriggerCommand&) {
    return NotImplemented<ReminderTrigger>("DismissReminderTrigger");
}

}  // namespace voicelife::timing
