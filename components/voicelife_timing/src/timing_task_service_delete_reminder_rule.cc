#include "voicelife/timing/timing_task_service.h"

namespace voicelife::timing {

Result<DeleteReminderRuleResult> DefaultTimingTaskService::DeleteReminderRule(
    const DeleteReminderRuleCommand& command) {
    if (command.reminder_rule_id.empty()) {
        return Result<DeleteReminderRuleResult>::Failure(ErrorCode::kInvalidArgument, "删除请求缺少提醒规则标识");
    }

    const auto disabled = store_.DisableReminderRule(command.reminder_rule_id, clock_.Now());
    if (!disabled.ok()) {
        return Result<DeleteReminderRuleResult>::Failure(disabled.status.code, disabled.status.message);
    }
    return Result<DeleteReminderRuleResult>::Success({
        .reminder_rule_id = command.reminder_rule_id,
        .status = ReminderRuleStatus::kDisabled,
        .affected_trigger_count = *disabled.value,
    });
}

}  // namespace voicelife::timing
