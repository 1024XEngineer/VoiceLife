#include "voicelife/schedule/schedule_service.h"

#include <cstddef>
#include <utility>

#include "schedule_create_helpers.h"
#include "schedule_mock_data.h"
#include "schedule_time_rules.h"

namespace voicelife::schedule {
namespace {

constexpr std::size_t kMaximumEventLength = 100;

}  // namespace

CreateScheduleResult ScheduleService::create_schedule(const CreateScheduleCommand& command) const {
    // 健壮性校验
    const std::string event = TrimScheduleText(command.event);
    if (event.empty()) return InvalidCreateScheduleResult("日程名称不能为空");
    if (ScheduleTextLength(event) > kMaximumEventLength) {
        return InvalidCreateScheduleResult("日程名称不能超过 100 个字符");
    }
    if (!command.start_time.has_value() && command.end_time.has_value()) {
        return InvalidCreateScheduleResult("日程提供结束时间时必须同时提供开始时间");
    }
    if (command.start_time.has_value() && command.end_time.has_value() && *command.end_time <= *command.start_time) {
        return InvalidCreateScheduleResult("日程结束时间必须晚于开始时间");
    }

    // 组装日程
    Schedule schedule{
        .id = 0,
        .event = event,
        .start_time = command.start_time,
        .end_time = command.end_time,
        .location = command.location,
        .notes = command.notes,
        .reminder_id = std::nullopt,
        .status = ScheduleStatus::kActive,
        .created_at = {},
        .updated_at = {},
    };

    // 搜集与当前日程冲突日程+临近日程
    std::vector<Schedule> conflicts;
    std::vector<Schedule> nearby_schedules;
    if (schedule.start_time.has_value()) {
        for (const Schedule& existing : LoadMockSchedulesForCreate()) {
            if (existing.status != ScheduleStatus::kActive || !existing.start_time.has_value()) continue;
            if (SchedulesConflict(schedule, existing)) {
                conflicts.push_back(existing);
            } else if (SchedulesAreNearby(schedule, existing)) {
                nearby_schedules.push_back(existing);
            }
        }
    }

    // 日程是否冲突
    if (!conflicts.empty() && !command.ignore_conflict) {
        return {
            .status = Status::Error(ErrorCode::kConflict, "日程时间与已有日程冲突"),
            .message = {},
            .schedule = std::nullopt,
            .conflicts = std::move(conflicts),
            .nearby_schedules = std::move(nearby_schedules),
            .error = "日程时间与已有日程冲突",
        };
    }

    // TODO：落库

    const std::string message = nearby_schedules.empty() ? "日程创建成功" : "日程创建成功，附近还有其他日程";
    return {
        .status = Status::Ok(),
        .message = message,
        .schedule = std::move(schedule),
        .conflicts = std::move(conflicts),
        .nearby_schedules = std::move(nearby_schedules),
        .error = {},
    };
}

DeleteScheduleResult ScheduleService::delete_schedule(const DeleteScheduleCommand& command) {
    // 健壮性判断
    if (command.schedule_id <= 0) {
        constexpr char kError[] = "日程 ID 必须为正整数";
        return {
            .status = Status::Error(ErrorCode::kInvalidArgument, kError),
            .schedule_id = command.schedule_id,
            .deleted = false,
            .error = kError,
        };
    }

    // mock 删除
    const Result<Schedule> cancelled = CancelMockSchedule(command.schedule_id);
    if (!cancelled.ok()) {
        return {
            .status = cancelled.status,
            .schedule_id = command.schedule_id,
            .deleted = false,
            .error = cancelled.status.message,
        };
    }

    return {
        .status = Status::Ok(),
        .schedule_id = command.schedule_id,
        .deleted = true,
        .error = {},
    };
}

}  // namespace voicelife::schedule
