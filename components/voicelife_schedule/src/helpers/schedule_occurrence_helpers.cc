#include "schedule_occurrence_helpers.h"

namespace voicelife::schedule {

// 先按 exception.schedule_id 读取已物化实例；关联失效时回退到 (rule_id, original_start_time) 查询。
Result<std::optional<Schedule>> FindMaterializedScheduleOccurrence(
    ScheduleRepository& repository, ScheduleRuleId rule_id, DateTime original_start_time,
    std::optional<ScheduleId> exception_schedule_id) {
    if (exception_schedule_id.has_value()) {
        const Result<Schedule> found = repository.FindById(*exception_schedule_id);
        if (!found.ok()) {
            if (found.status.code == ErrorCode::kNotFound) {
                return Result<std::optional<Schedule>>::Success(std::nullopt);
            }
            return Result<std::optional<Schedule>>::Failure(found.status.code, found.status.message);
        }
        return Result<std::optional<Schedule>>::Success(*found.value);
    }

    QueryScheduleCommand query;
    query.rule_id = rule_id;
    query.start_from = original_start_time;
    query.start_to = original_start_time;
    query.status = ScheduleStatusFilter::kAll;
    query.limit = 1;
    const Result<std::vector<Schedule>> loaded = repository.Find(query);
    if (!loaded.ok()) return Result<std::optional<Schedule>>::Failure(loaded.status.code, loaded.status.message);
    return Result<std::optional<Schedule>>::Success(
        loaded.value->empty() ? std::nullopt : std::optional<Schedule>{loaded.value->front()});
}

}  // namespace voicelife::schedule
