#include <algorithm>
#include <string>
#include <utility>

#include "voicelife/timing/timing_task_service.h"

namespace voicelife::timing {

Result<ReminderTrigger> DefaultTimingTaskService::DismissReminderTrigger(const DismissReminderTriggerCommand& command) {
    if (command.reminder_trigger_id.empty()) {
        return Result<ReminderTrigger>::Failure(ErrorCode::kInvalidArgument, "提醒关闭请求缺少触发标识");
    }

    const auto triggers = store_.ListTriggers();
    if (!triggers.ok()) {
        return Result<ReminderTrigger>::Failure(triggers.status.code, triggers.status.message);
    }

    const auto found = std::find_if(triggers.value->begin(), triggers.value->end(), [&command](const auto& trigger) {
        return trigger.id == command.reminder_trigger_id;
    });
    if (found == triggers.value->end()) {
        return Result<ReminderTrigger>::Failure(ErrorCode::kNotFound, "提醒触发不存在");
    }
    if (found->type != ReminderType::kStrong) {
        return Result<ReminderTrigger>::Failure(ErrorCode::kInvalidArgument, "弱提醒不能关闭");
    }
    if (!CanTransition(found->type, found->status, ReminderTriggerStatus::kDismissed)) {
        return Result<ReminderTrigger>::Failure(ErrorCode::kConflict, "当前状态的提醒不能关闭");
    }

    const int64_t now = clock_.Now();
    ReminderTrigger updated = *found;
    updated.status = ReminderTriggerStatus::kDismissed;
    updated.last_action_at = now;
    updated.updated_at = now;

    const TimingEvent event{
        .event_type = TimingEventType::kReminderDismissed,
        .event_id = updated.id + "/dismiss",
        .task_id = updated.task_id,
        .instance_id = updated.instance_id,
        .reminder_rule_id = updated.reminder_rule_id,
        .reminder_trigger_id = updated.id,
        .planned_at = updated.planned_trigger_at,
        .trigger_at = updated.actual_trigger_at,
        .status = TimingEventStatus::kDismissed,
        .payload = updated.payload,
        .occurred_at = now,
    };
    const Status saved = store_.UpdateReminderTriggerWithEvent({
        .trigger = updated,
        .event = event,
        .expected_status = found->status,
        .expected_snooze_count = found->snooze_count,
        .expected_updated_at = found->updated_at,
    });
    if (!saved.ok()) {
        return Result<ReminderTrigger>::Failure(saved.code, saved.message);
    }
    return Result<ReminderTrigger>::Success(std::move(updated));
}

}  // namespace voicelife::timing
