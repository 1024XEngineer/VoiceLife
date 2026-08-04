#include <concepts>
#include <type_traits>

#include "voicelife/timing/timing_task_service.h"

using namespace voicelife;
using namespace voicelife::timing;

template <typename Service>
concept TimingTaskServiceContract = requires(
    Service& service, const RegisterTimerTaskCommand& register_command, const UpdateTimerTaskCommand& update_command,
    const CancelTimerTaskCommand& cancel_command, const UpsertReminderRulesCommand& upsert_rules_command,
    const DeleteReminderRuleCommand& delete_rule_command, const CalendarViewQuery& calendar_query,
    const ReminderTriggerQuery& trigger_query, const SnoozeReminderTriggerCommand& snooze_command,
    const DismissReminderTriggerCommand& dismiss_command) {
    { service.RegisterTimerTask(register_command) } -> std::same_as<Result<RegisterTimerTaskResult>>;
    { service.UpdateTimerTask(update_command) } -> std::same_as<Result<UpdateTimerTaskResult>>;
    { service.CancelTimerTask(cancel_command) } -> std::same_as<Result<CancelTimerTaskResult>>;
    { service.UpsertReminderRules(upsert_rules_command) } -> std::same_as<Result<UpsertReminderRulesResult>>;
    { service.DeleteReminderRule(delete_rule_command) } -> std::same_as<Result<DeleteReminderRuleResult>>;
    { service.ListCalendarView(calendar_query) } -> std::same_as<Result<CalendarView>>;
    { service.ListReminderTriggers(trigger_query) } -> std::same_as<Result<ReminderTriggerPage>>;
    { service.SnoozeReminderTrigger(snooze_command) } -> std::same_as<Result<ReminderTrigger>>;
    { service.DismissReminderTrigger(dismiss_command) } -> std::same_as<Result<ReminderTrigger>>;
};

static_assert(std::has_virtual_destructor_v<TimingTaskService>);
static_assert(TimingTaskServiceContract<TimingTaskService>);

int main() { return 0; }
