#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "voicelife/timing/timing_task_service.h"

namespace voicelife::timing {
namespace {

constexpr int64_t kSecondsPerMinute = 60;

}  // namespace

Result<ReminderTrigger> DefaultTimingTaskService::SnoozeReminderTrigger(const SnoozeReminderTriggerCommand& command) {
    if (command.reminder_trigger_id.empty() || command.delay_minutes <= 0) {
        return Result<ReminderTrigger>::Failure(ErrorCode::kInvalidArgument, "提醒推迟请求标识或延迟无效");
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
        return Result<ReminderTrigger>::Failure(ErrorCode::kInvalidArgument, "弱提醒不能推迟");
    }
    if (!CanTransition(found->type, found->status, ReminderTriggerStatus::kSnoozed)) {
        return Result<ReminderTrigger>::Failure(ErrorCode::kConflict, "只有 triggered 状态的提醒可以推迟");
    }
    if (found->snooze_count >= found->max_snooze_count) {
        return Result<ReminderTrigger>::Failure(ErrorCode::kConflict, "提醒推迟次数已达到上限");
    }

    const int64_t now = clock_.Now();
    const int64_t delay_seconds = static_cast<int64_t>(command.delay_minutes) * kSecondsPerMinute;
    if (now > std::numeric_limits<int64_t>::max() - delay_seconds) {
        return Result<ReminderTrigger>::Failure(ErrorCode::kInvalidArgument, "提醒推迟时间超出范围");
    }

    ReminderTrigger updated = *found;
    updated.status = ReminderTriggerStatus::kSnoozed;
    ++updated.snooze_count;
    updated.actual_trigger_at = now + delay_seconds;
    updated.last_action_at = now;
    updated.updated_at = now;

    const TimingEvent event{
        .event_type = TimingEventType::kReminderSnoozed,
        .event_id = updated.id + "/snooze/" + std::to_string(updated.snooze_count),
        .task_id = updated.task_id,
        .instance_id = updated.instance_id,
        .reminder_rule_id = updated.reminder_rule_id,
        .reminder_trigger_id = updated.id,
        .planned_at = updated.planned_trigger_at,
        .trigger_at = updated.actual_trigger_at,
        .status = TimingEventStatus::kSnoozed,
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
