#include "voicelife/application/calendar_application.h"

#include <utility>

namespace voicelife::application {
namespace {

Result<CreateScheduleOutcome> DuplicateOutcome(const StoredCalendarEntry& entry) {
    return Result<CreateScheduleOutcome>::Success({
        .schedule_id = entry.schedule.id,
        .timing_task_id = entry.timing_task.id,
        .duplicate = true,
        .notification_accepted = false,
    });
}

}  // namespace

Result<CreateScheduleOutcome> CalendarApplication::CreateSchedule(const schedule::CreateScheduleCommand& command) {
    const auto existing = store_.FindByRequestId(command.request_id);
    if (!existing.ok()) {
        return Result<CreateScheduleOutcome>::Failure(existing.status.code, existing.status.message);
    }
    if (existing.value->has_value()) {
        return DuplicateOutcome(existing.value->value());
    }

    const int64_t now = ids_.Now();
    auto schedule = schedule_policy_.Create(command, ids_.Next("schedule"), now);
    if (!schedule.ok()) {
        return Result<CreateScheduleOutcome>::Failure(schedule.status.code, schedule.status.message);
    }

    timing::RegisterTimingTaskCommand timing_command{
        .schedule_id = schedule.value->id,
        .starts_at = schedule.value->starts_at,
        .time_zone = schedule.value->time_zone,
    };
    auto task = timing_policy_.Register(timing_command, ids_.Next("task"), now);
    if (!task.ok()) {
        return Result<CreateScheduleOutcome>::Failure(task.status.code, task.status.message);
    }

    StoredCalendarEntry entry{.schedule = *schedule.value, .timing_task = *task.value};
    const Status saved = store_.SaveScheduleWithTimingTask(entry);
    if (!saved.ok()) {
        if (saved.code == ErrorCode::kConflict) {
            const auto concurrent = store_.FindByRequestId(command.request_id);
            if (!concurrent.ok()) {
                return Result<CreateScheduleOutcome>::Failure(concurrent.status.code, concurrent.status.message);
            }
            if (concurrent.value->has_value()) {
                return DuplicateOutcome(concurrent.value->value());
            }
        }
        return Result<CreateScheduleOutcome>::Failure(saved.code, saved.message);
    }

    NotificationIntent intent{
        .event_id = ids_.Next("event"),
        .correlation_id = command.request_id,
        .kind = "schedule.created",
        .schedule_id = entry.schedule.id,
        .summary = entry.schedule.title,
    };
    const Status notified = notifications_.Publish(intent);

    return Result<CreateScheduleOutcome>::Success({
        .schedule_id = entry.schedule.id,
        .timing_task_id = entry.timing_task.id,
        .duplicate = false,
        .notification_accepted = notified.ok(),
    });
}

}  // namespace voicelife::application
