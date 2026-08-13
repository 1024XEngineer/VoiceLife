#include "schedule_time_rules.h"

#include <chrono>
#include <utility>

namespace voicelife::schedule {
namespace {

constexpr auto kNearbyWindow = std::chrono::minutes{15};

}  // namespace

// 先区分时间点和时间区间，再按半开区间重叠判断两个日程是否冲突。
bool SchedulesConflict(const Schedule& left, const Schedule& right) {
    const DateTime left_start = *left.start_time;
    const DateTime right_start = *right.start_time;
    const DateTime left_end = ScheduleRangeEnd(left);
    const DateTime right_end = ScheduleRangeEnd(right);
    const bool left_is_point = !left.end_time.has_value();
    const bool right_is_point = !right.end_time.has_value();

    if (left_is_point && right_is_point) return left_start == right_start;
    if (left_is_point) return left_start >= right_start && left_start < right_end;
    if (right_is_point) return right_start >= left_start && right_start < left_end;
    return left_start < right_end && right_start < left_end;
}

// 对未冲突的日程，按开始时间是否落在 15 分钟窗口内判断“临近”。
bool SchedulesAreNearby(const Schedule& left, const Schedule& right) {
    // 临近日程围绕开始时间：两个开始时间相差不超过 15 分钟。
    const DateTime left_start = *left.start_time;
    const DateTime right_start = *right.start_time;
    if (left_start <= right_start) return right_start - left_start <= kNearbyWindow;
    return left_start - right_start <= kNearbyWindow;
}

DateTime ScheduleRangeEnd(const Schedule& schedule) { return schedule.end_time.value_or(*schedule.start_time); }

std::pair<DateTime, DateTime> ScheduleNearbyWindow(const Schedule& schedule) {
    const DateTime start = *schedule.start_time;
    const DateTime end = ScheduleRangeEnd(schedule);
    return {start - kNearbyWindow, end + kNearbyWindow};
}

std::vector<Schedule> FindConflictingSchedules(const Schedule& candidate, const std::vector<Schedule>& schedules,
                                               std::optional<ScheduleRuleId> ignored_rule_id) {
    // 从候选集合中筛出真正重叠的 active 日程；调用方可按规则忽略自身实例。
    std::vector<Schedule> conflicts;
    for (const Schedule& existing : schedules) {
        if (existing.id == candidate.id || existing.status != ScheduleStatus::kActive ||
            !existing.start_time.has_value()) {
            continue;
        }
        if (ignored_rule_id.has_value() && existing.rule_id == ignored_rule_id) continue;
        if (SchedulesConflict(candidate, existing)) conflicts.push_back(existing);
    }
    return conflicts;
}

std::vector<Schedule> FindNearbySchedules(const Schedule& candidate, const std::vector<Schedule>& schedules) {
    // 只返回不冲突但时间上临近的日程，供创建结果做提醒。
    std::vector<Schedule> nearby;
    for (const Schedule& existing : schedules) {
        if (existing.id == candidate.id || existing.status != ScheduleStatus::kActive ||
            !existing.start_time.has_value()) {
            continue;
        }
        if (!SchedulesConflict(candidate, existing) && SchedulesAreNearby(candidate, existing)) {
            nearby.push_back(existing);
        }
    }
    return nearby;
}

}  // namespace voicelife::schedule
