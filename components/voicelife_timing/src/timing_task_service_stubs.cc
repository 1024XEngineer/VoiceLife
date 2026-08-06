#include <string>

#include "voicelife/timing/timing_task_service.h"

namespace voicelife::timing {
namespace {

template <typename T>
Result<T> NotImplemented(const char* operation) {
    return Result<T>::Failure(ErrorCode::kUnavailable, std::string(operation) + " 尚未实现");
}

}  // namespace

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
