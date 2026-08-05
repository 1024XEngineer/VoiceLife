#include "voicelife/schedule/schedule_service.h"

#include <chrono>
#include <cstddef>
#include <utility>

#include "schedule_create_helpers.h"
#include "schedule_mock_data.h"
#include "schedule_time_rules.h"
#include "schedule_update_helpers.h"

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
        for (const Schedule& existing : LoadMockSchedules()) {
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

UpdateScheduleResult ScheduleService::update_schedule(const UpdateScheduleCommand& command) {
    if (command.schedule_id <= 0) return InvalidUpdateScheduleResult("日程 ID 必须大于零");

    // 根据调用方预先确认的 ID 查询目标日程；数据库接入前暂由固定模拟数据提供。
    std::vector<Schedule> schedules = LoadMockSchedules();
    auto target = schedules.end();
    for (auto current = schedules.begin(); current != schedules.end(); ++current) {
        if (current->id == command.schedule_id) {
            target = current;
            break;
        }
    }
    if (target == schedules.end()) {
        const std::string error = "未找到要修改的日程";
        return {
            .status = Status::Error(ErrorCode::kNotFound, error),
            .message = {},
            .schedule = std::nullopt,
            .conflicts = {},
            .error = error,
        };
    }

    const bool has_update = command.event.has_value() || command.start_time.has_value() ||
                            command.end_time.has_value() || command.location.has_value() || command.notes.has_value() ||
                            command.reminder_id.has_value() || command.status.has_value();
    if (!has_update) return InvalidUpdateScheduleResult("至少需要提供一个要修改的字段");

    // 在原日程副本上合并本次提供的字段，未提供的字段保持不变，显式空值用于清空字段。
    Schedule updated = *target;
    if (command.event.has_value()) {
        updated.event = TrimScheduleText(*command.event);
        if (updated.event.empty()) return InvalidUpdateScheduleResult("日程名称不能为空");
        if (ScheduleTextLength(updated.event) > kMaximumEventLength) {
            return InvalidUpdateScheduleResult("日程名称不能超过 100 个字符");
        }
    }
    ApplyNullableUpdate(command.start_time, updated.start_time);
    ApplyNullableUpdate(command.end_time, updated.end_time);
    ApplyNullableUpdate(command.location, updated.location);
    ApplyNullableUpdate(command.notes, updated.notes);
    ApplyNullableUpdate(command.reminder_id, updated.reminder_id);
    if (command.status.has_value()) {
        if (!IsSupportedScheduleStatus(*command.status)) return InvalidUpdateScheduleResult("不支持的日程状态");
        updated.status = *command.status;
    }

    // 字段合并完成后校验完整日程，避免单独校验增量字段时遗漏原有值之间的约束。
    if (!updated.start_time.has_value() && updated.end_time.has_value()) {
        return InvalidUpdateScheduleResult("日程提供结束时间时必须同时提供开始时间");
    }
    if (updated.start_time.has_value() && updated.end_time.has_value() && *updated.end_time <= *updated.start_time) {
        return InvalidUpdateScheduleResult("日程结束时间必须晚于开始时间");
    }

    // 仅有效且具有开始时间的日程参与冲突检测，并排除正在修改的日程自身。
    std::vector<Schedule> conflicts;
    if (updated.status == ScheduleStatus::kActive && updated.start_time.has_value()) {
        for (const Schedule& existing : schedules) {
            if (existing.id == updated.id || existing.status != ScheduleStatus::kActive ||
                !existing.start_time.has_value()) {
                continue;
            }
            if (SchedulesConflict(updated, existing)) conflicts.push_back(existing);
        }
    }
    if (!conflicts.empty() && !command.ignore_conflict) {
        const std::string error = "修改后的日程时间与已有日程冲突";
        return {
            .status = Status::Error(ErrorCode::kConflict, error),
            .message = {},
            .schedule = std::nullopt,
            .conflicts = std::move(conflicts),
            .error = error,
        };
    }

    updated.updated_at = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());

    // TODO(#134): 在数据库适配器中以原子操作持久化日程变更；冲突返回分支不得执行写入。

    // 忽略冲突时仍返回冲突列表，便于调用方在修改成功后向用户说明潜在影响。
    return {
        .status = Status::Ok(),
        .message = conflicts.empty() ? "日程修改成功" : "日程修改成功，已忽略时间冲突",
        .schedule = std::move(updated),
        .conflicts = std::move(conflicts),
        .error = {},
    };
}

}  // namespace voicelife::schedule
