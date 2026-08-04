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

Result<RegisterTimerTaskResult> DefaultTimingTaskService::RegisterTimerTask(
    const RegisterTimerTaskCommand& command) {
    const RegisterTimingTaskCommand policy_command{
        .schedule_id = command.schedule_id,
        .starts_at = command.start_at,
        .time_zone = command.time_zone,
    };

    auto task = TimingPolicy{}.Register(policy_command, ids_.NextTaskId(), clock_.Now());
    if (!task.ok()) {
        return Result<RegisterTimerTaskResult>::Failure(task.status.code, task.status.message);
    }

    const Status saved = store_.SaveTask(*task.value);
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

Result<UpsertReminderRulesResult> DefaultTimingTaskService::UpsertReminderRules(
    const UpsertReminderRulesCommand&) {
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
