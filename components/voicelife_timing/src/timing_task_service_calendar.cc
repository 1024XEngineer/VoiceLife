#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

#include "voicelife/timing/occurrence_planner.h"
#include "voicelife/timing/timing_task_service.h"

namespace voicelife::timing {
namespace {

constexpr int64_t kSecondsPerDay = 24 * 60 * 60;
constexpr int64_t kMinimumRangeStart = (std::numeric_limits<int64_t>::min() / kSecondsPerDay) * kSecondsPerDay;

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

        const auto planned_occurrences = PlanOccurrences(task, query.range_start, query.range_end);
        if (!planned_occurrences.ok()) {
            return Result<CalendarView>::Failure(planned_occurrences.status.code, planned_occurrences.status.message);
        }
        for (const int64_t planned_at : *planned_occurrences.value) {
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
