#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "voicelife/timing/timing_task_service.h"

namespace voicelife::timing {
namespace {

bool InRange(const ReminderTrigger& trigger, const ReminderTriggerQuery& query) {
    return trigger.actual_trigger_at >= *query.range_start && trigger.actual_trigger_at < *query.range_end;
}

int64_t SortKey(const ReminderTrigger& trigger, TriggerSortBy sort_by) {
    switch (sort_by) {
        case TriggerSortBy::kActualTriggerAt:
            return trigger.actual_trigger_at;
        case TriggerSortBy::kPlannedTriggerAt:
            return trigger.planned_trigger_at;
        case TriggerSortBy::kCreatedAt:
            return trigger.created_at;
    }
    return trigger.actual_trigger_at;
}

}  // namespace

Result<ReminderTriggerPage> DefaultTimingTaskService::ListReminderTriggers(const ReminderTriggerQuery& query) {
    const bool has_time_range = query.range_start.has_value() || query.range_end.has_value();
    const bool has_filter = !query.task_id.empty() || !query.instance_id.empty() || !query.schedule_id.empty() ||
                            query.type.has_value() || query.status.has_value() || has_time_range;
    if (!has_filter || !query.range_start.has_value() != !query.range_end.has_value() ||
        (has_time_range && *query.range_start >= *query.range_end) || query.page < 1 || query.page_size < 1 ||
        query.page_size > 100) {
        return Result<ReminderTriggerPage>::Failure(ErrorCode::kInvalidArgument, "提醒触发查询条件或分页参数无效");
    }

    if (!query.task_id.empty()) {
        const auto task = store_.FindTask(query.task_id);
        if (!task.ok()) {
            return Result<ReminderTriggerPage>::Failure(task.status.code, task.status.message);
        }
    }
    if (!query.instance_id.empty()) {
        const auto instance = store_.FindInstance(query.instance_id);
        if (!instance.ok()) {
            return Result<ReminderTriggerPage>::Failure(instance.status.code, instance.status.message);
        }
    }

    std::unordered_set<TimingTaskId> schedule_task_ids;
    if (!query.schedule_id.empty()) {
        const auto tasks = store_.ListTasks();
        if (!tasks.ok()) {
            return Result<ReminderTriggerPage>::Failure(tasks.status.code, tasks.status.message);
        }
        for (const auto& task : *tasks.value) {
            if (task.schedule_id == query.schedule_id) {
                schedule_task_ids.insert(task.id);
            }
        }
        if (schedule_task_ids.empty()) {
            return Result<ReminderTriggerPage>::Failure(ErrorCode::kNotFound, "日程未关联定时任务");
        }
    }

    const auto triggers = store_.ListTriggers();
    if (!triggers.ok()) {
        return Result<ReminderTriggerPage>::Failure(triggers.status.code, triggers.status.message);
    }

    std::vector<ReminderTrigger> result;
    for (const auto& trigger : *triggers.value) {
        if ((!query.task_id.empty() && trigger.task_id != query.task_id) ||
            (!query.instance_id.empty() && trigger.instance_id != query.instance_id) ||
            (!query.schedule_id.empty() && !schedule_task_ids.contains(trigger.task_id)) ||
            (query.type.has_value() && trigger.type != *query.type) ||
            (query.status.has_value() && trigger.status != *query.status) ||
            (has_time_range && !InRange(trigger, query))) {
            continue;
        }
        result.push_back(trigger);
    }

    std::sort(result.begin(), result.end(), [&query](const ReminderTrigger& left, const ReminderTrigger& right) {
        const int64_t left_key = SortKey(left, query.sort_by);
        const int64_t right_key = SortKey(right, query.sort_by);
        if (left_key != right_key) {
            return query.sort_order == SortOrder::kAscending ? left_key < right_key : left_key > right_key;
        }
        return left.id < right.id;
    });

    const int total = static_cast<int>(result.size());
    const int64_t offset = static_cast<int64_t>(query.page - 1) * query.page_size;
    const int begin = offset >= total ? total : static_cast<int>(offset);
    const int end = std::min<int64_t>(total, static_cast<int64_t>(begin) + query.page_size);
    return Result<ReminderTriggerPage>::Success({
        .reminder_triggers = std::vector<ReminderTrigger>(result.begin() + begin, result.begin() + end),
        .total = total,
        .page = query.page,
        .page_size = query.page_size,
        .has_more = end < total,
    });
}

}  // namespace voicelife::timing
