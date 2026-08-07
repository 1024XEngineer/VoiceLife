#include "voicelife/timing/occurrence_planner.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace voicelife::timing {
namespace {

constexpr int64_t kSecondsPerDay = 24 * 60 * 60;
constexpr int64_t kUtcPlusEightOffsetSeconds = 8 * 60 * 60;
constexpr int64_t kMinimumRangeStart = (std::numeric_limits<int64_t>::min() / kSecondsPerDay) * kSecondsPerDay;

int64_t FloorDiv(int64_t value, int64_t divisor) {
    const int64_t quotient = value / divisor;
    const int64_t remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

struct CivilDate {
    int year;
    unsigned month;
    unsigned day;
};

CivilDate CivilFromDays(int64_t days_since_epoch) {
    days_since_epoch += 719468;
    const int64_t era = (days_since_epoch >= 0 ? days_since_epoch : days_since_epoch - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(days_since_epoch - era * 146097);
    const unsigned year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int year = static_cast<int>(year_of_era) + static_cast<int>(era) * 400;
    const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned month_part = (5 * day_of_year + 2) / 153;
    const unsigned day = day_of_year - (153 * month_part + 2) / 5 + 1;
    const unsigned month = month_part < 10 ? month_part + 3U : month_part - 9U;
    return {year + (month <= 2), month, day};
}

bool Contains(const std::vector<int>& values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

int Weekday(int64_t timestamp) {
    const int64_t days = FloorDiv(timestamp, kSecondsPerDay);
    return static_cast<int>(((days + 3) % 7 + 7) % 7 + 1);  // Monday=1, Sunday=7.
}

bool IsGeneratedDate(const TimingTask& task, int64_t local_timestamp, int64_t local_start_at) {
    const auto& rule = task.recurrence;
    switch (rule.frequency) {
        case RecurrenceFrequency::kNone:
            return local_timestamp == task.start_at;
        case RecurrenceFrequency::kDay:
            return true;
        case RecurrenceFrequency::kWeek:
            return rule.by_weekdays.empty()
                       ? (FloorDiv(local_timestamp, kSecondsPerDay) - FloorDiv(local_start_at, kSecondsPerDay)) % 7 == 0
                       : Contains(rule.by_weekdays, Weekday(local_timestamp));
        case RecurrenceFrequency::kMonth: {
            const CivilDate anchor = CivilFromDays(FloorDiv(local_start_at, kSecondsPerDay));
            const CivilDate date = CivilFromDays(FloorDiv(local_timestamp, kSecondsPerDay));
            return Contains(rule.by_month_days, static_cast<int>(date.day)) ||
                   (rule.by_month_days.empty() && date.day == anchor.day);
        }
        case RecurrenceFrequency::kYear: {
            const CivilDate anchor = CivilFromDays(FloorDiv(local_start_at, kSecondsPerDay));
            const CivilDate date = CivilFromDays(FloorDiv(local_timestamp, kSecondsPerDay));
            const bool month_matches = rule.by_months.empty() ? date.month == anchor.month
                                                              : Contains(rule.by_months, static_cast<int>(date.month));
            const bool day_matches = rule.by_month_days.empty()
                                         ? date.day == anchor.day
                                         : Contains(rule.by_month_days, static_cast<int>(date.day));
            return month_matches && day_matches;
        }
    }
    return false;
}

std::optional<int64_t> AddOffset(int64_t value, int64_t offset) {
    if (offset > 0 && value > std::numeric_limits<int64_t>::max() - offset) {
        return std::nullopt;
    }
    return value + offset;
}

std::optional<int64_t> SubtractOffset(int64_t value, int64_t offset) {
    if (offset > 0 && value < std::numeric_limits<int64_t>::min() + offset) {
        return std::nullopt;
    }
    return value - offset;
}

Result<std::vector<int64_t>> ExpandRecurringTask(const TimingTask& task, int64_t range_start, int64_t range_end) {
    if (task.time_zone != "+08:00") {
        return Result<std::vector<int64_t>>::Failure(ErrorCode::kUnavailable, "周期规划暂仅支持 +08:00 时区");
    }

    const auto local_range_start = AddOffset(range_start, kUtcPlusEightOffsetSeconds);
    const auto local_range_end = AddOffset(range_end, kUtcPlusEightOffsetSeconds);
    const auto local_start_at = AddOffset(task.start_at, kUtcPlusEightOffsetSeconds);
    if (!local_range_start.has_value() || !local_range_end.has_value() || !local_start_at.has_value()) {
        return Result<std::vector<int64_t>>::Success(std::vector<int64_t>{});
    }

    std::vector<int64_t> planned_times;
    const int64_t first_day = FloorDiv(*local_range_start, kSecondsPerDay) * kSecondsPerDay;
    const int64_t last_day =
        *local_range_end > std::numeric_limits<int64_t>::min() ? *local_range_end - 1 : *local_range_end;
    const int64_t last_day_start = FloorDiv(last_day, kSecondsPerDay) * kSecondsPerDay;
    const int64_t anchor_day = FloorDiv(*local_start_at, kSecondsPerDay) * kSecondsPerDay;
    const int64_t time_of_day = *local_start_at - anchor_day;

    for (int64_t day = first_day; day <= last_day_start && day <= std::numeric_limits<int64_t>::max() - kSecondsPerDay;
         day += kSecondsPerDay) {
        const int64_t candidate_local = day + time_of_day;
        const auto candidate = SubtractOffset(candidate_local, kUtcPlusEightOffsetSeconds);
        if (candidate.has_value() && *candidate >= range_start && *candidate < range_end &&
            *candidate >= task.start_at && (task.effective_until == 0 || *candidate < task.effective_until) &&
            IsGeneratedDate(task, candidate_local, *local_start_at)) {
            planned_times.push_back(*candidate);
        }
    }
    return Result<std::vector<int64_t>>::Success(std::move(planned_times));
}

}  // namespace

Result<std::vector<int64_t>> PlanOccurrences(const TimingTask& task, int64_t range_start, int64_t range_end) {
    if (range_start < kMinimumRangeStart || range_start >= range_end) {
        return Result<std::vector<int64_t>>::Failure(ErrorCode::kInvalidArgument, "occurrence 规划范围无效");
    }
    if (task.recurrence.frequency == RecurrenceFrequency::kNone) {
        if (task.start_at >= range_start && task.start_at < range_end) {
            return Result<std::vector<int64_t>>::Success({task.start_at});
        }
        return Result<std::vector<int64_t>>::Success(std::vector<int64_t>{});
    }
    return ExpandRecurringTask(task, range_start, range_end);
}

}  // namespace voicelife::timing
