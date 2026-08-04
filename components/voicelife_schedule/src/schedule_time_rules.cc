#include "schedule_time_rules.h"

#include <chrono>

namespace voicelife::schedule {
namespace {

constexpr auto kNearbyWindow = std::chrono::minutes{15};

/** @brief 返回日程的区间终点；无结束时间的日程按单个时间点处理。 */
DateTime RangeEnd(const Schedule& schedule) { return schedule.end_time.value_or(*schedule.start_time); }

}  // namespace

bool SchedulesConflict(const Schedule& left, const Schedule& right) {
    const DateTime left_start = *left.start_time;
    const DateTime right_start = *right.start_time;
    const DateTime left_end = RangeEnd(left);
    const DateTime right_end = RangeEnd(right);
    const bool left_is_point = !left.end_time.has_value();
    const bool right_is_point = !right.end_time.has_value();

    if (left_is_point && right_is_point) return left_start == right_start;
    if (left_is_point) return left_start >= right_start && left_start < right_end;
    if (right_is_point) return right_start >= left_start && right_start < left_end;
    return left_start < right_end && right_start < left_end;
}

bool SchedulesAreNearby(const Schedule& left, const Schedule& right) {
    const DateTime left_start = *left.start_time;
    const DateTime right_start = *right.start_time;
    const DateTime left_end = RangeEnd(left);
    const DateTime right_end = RangeEnd(right);

    if (left_end <= right_start) return right_start - left_end <= kNearbyWindow;
    if (right_end <= left_start) return left_start - right_end <= kNearbyWindow;
    return false;
}

}  // namespace voicelife::schedule
