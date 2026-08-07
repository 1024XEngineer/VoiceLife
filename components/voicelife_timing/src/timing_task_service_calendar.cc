#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "voicelife/timing/timing_task_service.h"

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

// MVP recurrence uses a fixed UTC+8 civil calendar; IANA timezone rules remain outside this scope.
struct CivilDate {
    int year;
    unsigned month;
    unsigned day;
};

CivilDate CivilFromDays(int64_t days_since_epoch) {
    days_since_epoch += 719468;
    const int64_t era = (days_since_epoch >= 0 ? days_since_epoch : days_since_epoch - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(days_since_epoch - era * 146097);
    const unsigned year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 365 - day_of_era / 146096) / 365;
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

bool IsGeneratedDate(const TimingTask& task, int64_t timestamp, int64_t local_start_at) {
    const auto& rule = task.recurrence;
    switch (rule.frequency) {
        case RecurrenceFrequency::kNone:
            return timestamp == task.start_at;
        case RecurrenceFrequency::kDay:
            return true;
        case RecurrenceFrequency::kWeek:
            return rule.by_weekdays.empty()
                       ? (FloorDiv(timestamp, kSecondsPerDay) - FloorDiv(local_start_at, kSecondsPerDay)) % 7 == 0
                       : Contains(rule.by_weekdays, Weekday(timestamp));
        case RecurrenceFrequency::kMonth: {
            const CivilDate anchor = CivilFromDays(FloorDiv(local_start_at, kSecondsPerDay));
            const CivilDate date = CivilFromDays(FloorDiv(timestamp, kSecondsPerDay));
            return Contains(rule.by_month_days, static_cast<int>(date.day)) ||
                   (rule.by_month_days.empty() && date.day == anchor.day);
        }
        case RecurrenceFrequency::kYear: {
            const CivilDate anchor = CivilFromDays(FloorDiv(local_start_at, kSecondsPerDay));
            const CivilDate date = CivilFromDays(FloorDiv(timestamp, kSecondsPerDay));
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

std::vector<int64_t> ExpandTask(const TimingTask& task, int64_t range_start, int64_t range_end) {
    std::vector<int64_t> planned_times;
    if (task.recurrence.frequency == RecurrenceFrequency::kNone) {
        if (task.start_at >= range_start && task.start_at < range_end) {
            planned_times.push_back(task.start_at);
        }
        return planned_times;
    }

    const int64_t offset = task.time_zone == "+08:00" ? kUtcPlusEightOffsetSeconds : 0;
    const auto local_range_start = AddOffset(range_start, offset);
    const auto local_range_end = AddOffset(range_end, offset);
    const auto local_start_at = AddOffset(task.start_at, offset);
    if (!local_range_start.has_value() || !local_range_end.has_value() || !local_start_at.has_value()) {
        return planned_times;
    }

    const int64_t first_day = FloorDiv(*local_range_start, kSecondsPerDay) * kSecondsPerDay;
    const int64_t last_day =
        *local_range_end > std::numeric_limits<int64_t>::min() ? *local_range_end - 1 : *local_range_end;
    const int64_t last_day_start = FloorDiv(last_day, kSecondsPerDay) * kSecondsPerDay;
    const int64_t anchor_day = FloorDiv(*local_start_at, kSecondsPerDay) * kSecondsPerDay;
    const int64_t time_of_day = *local_start_at - anchor_day;

    for (int64_t day = first_day; day <= last_day_start && day <= std::numeric_limits<int64_t>::max() - kSecondsPerDay;
         day += kSecondsPerDay) {
        const int64_t candidate_local = day + time_of_day;
        const int64_t candidate = candidate_local - offset;
        if (candidate >= range_start && candidate < range_end && candidate >= task.start_at &&
            (task.effective_until == 0 || candidate < task.effective_until) &&
            IsGeneratedDate(task, candidate_local, *local_start_at)) {
            planned_times.push_back(candidate);
        }
    }
    return planned_times;
}

CalendarOccurrence ToOccurrence(const TimingTask& task, const TimerInstance& instance) {
    const int64_t planned_start = instance.override_fields.start_at.value_or(instance.planned_at);
    const int64_t planned_end = instance.override_fields.end_at.value_or(instance.planned_end_at);
    return {
        .occurrence_id = instance.id,
        .schedule_id = task.schedule_id,
        .task_id = task.id,
        .instance_id = instance.id,
        .planned_start_at = planned_start,
        .planned_end_at = planned_end,
        .actual_trigger_at = instance.actual_trigger_at,
        .status = instance.status,
        .is_recurring = task.recurrence.frequency != RecurrenceFrequency::kNone,
        .is_exception = true,
        .override_fields = instance.override_fields,
    };
}

CalendarOccurrence ToOccurrence(const TimingTask& task, int64_t planned_at) {
    return {
        .occurrence_id = task.id + "@" + std::to_string(planned_at),
        .schedule_id = task.schedule_id,
        .task_id = task.id,
        .instance_id = {},
        .planned_start_at = planned_at,
        .planned_end_at = planned_at,
        .actual_trigger_at = 0,
        .status = TimerInstanceStatus::kPending,
        .is_recurring = task.recurrence.frequency != RecurrenceFrequency::kNone,
        .is_exception = false,
        .override_fields = {},
    };
}

bool InRange(const CalendarOccurrence& occurrence, const CalendarViewQuery& query) {
    return occurrence.planned_start_at >= query.range_start && occurrence.planned_start_at < query.range_end;
}

}  // namespace

Result<CalendarView> DefaultTimingTaskService::ListCalendarView(const CalendarViewQuery& query) {
    if (query.range_start < kMinimumRangeStart || query.range_start >= query.range_end || query.page < 1 ||
        query.page_size < 1) {
        return Result<CalendarView>::Failure(ErrorCode::kInvalidArgument, "日历查询范围或分页参数无效");
    }

    const auto tasks = store_.ListTasks();
    if (!tasks.ok()) {
        return Result<CalendarView>::Failure(tasks.status.code, tasks.status.message);
    }

    std::vector<CalendarOccurrence> occurrences;
    for (const auto& task : *tasks.value) {
        if (task.status != TimingTaskStatus::kActive || task.deleted_at != 0 ||
            (!query.schedule_id.empty() && task.schedule_id != query.schedule_id)) {
            continue;
        }

        if (task.recurrence.frequency != RecurrenceFrequency::kNone && task.time_zone != "+08:00") {
            return Result<CalendarView>::Failure(ErrorCode::kUnavailable, "周期日历展开暂仅支持 +08:00 时区");
        }

        const auto instances = store_.ListInstances(task.id);
        if (!instances.ok()) {
            return Result<CalendarView>::Failure(instances.status.code, instances.status.message);
        }

        std::unordered_set<int64_t> materialized_planned_at;
        for (const auto& instance : *instances.value) {
            if (instance.deleted_at != 0) {
                continue;
            }
            materialized_planned_at.insert(instance.planned_at);
            const CalendarOccurrence occurrence = ToOccurrence(task, instance);
            if ((!query.status.has_value() || occurrence.status == *query.status) && InRange(occurrence, query)) {
                occurrences.push_back(occurrence);
            }
        }

        for (const int64_t planned_at : ExpandTask(task, query.range_start, query.range_end)) {
            if (materialized_planned_at.contains(planned_at)) {
                continue;
            }
            const CalendarOccurrence occurrence = ToOccurrence(task, planned_at);
            if (!query.status.has_value() || occurrence.status == *query.status) {
                occurrences.push_back(occurrence);
            }
        }
    }

    const auto compare = [&query](const CalendarOccurrence& left, const CalendarOccurrence& right) {
        const auto left_key =
            query.sort_by == CalendarSortBy::kActualTriggerAt ? left.actual_trigger_at : left.planned_start_at;
        const auto right_key =
            query.sort_by == CalendarSortBy::kActualTriggerAt ? right.actual_trigger_at : right.planned_start_at;
        if (left_key != right_key) {
            return query.sort_order == SortOrder::kAscending ? left_key < right_key : left_key > right_key;
        }
        return left.occurrence_id < right.occurrence_id;
    };
    std::sort(occurrences.begin(), occurrences.end(), compare);

    const int total = static_cast<int>(occurrences.size());
    const int64_t offset = static_cast<int64_t>(query.page - 1) * query.page_size;
    const int begin = offset >= total ? total : static_cast<int>(offset);
    const int end = std::min<int64_t>(total, static_cast<int64_t>(begin) + query.page_size);
    CalendarView result{
        .occurrences = std::vector<CalendarOccurrence>(occurrences.begin() + begin, occurrences.begin() + end),
        .total = total,
        .page = query.page,
        .page_size = query.page_size,
        .has_more = end < total,
    };
    return Result<CalendarView>::Success(std::move(result));
}

}  // namespace voicelife::timing
