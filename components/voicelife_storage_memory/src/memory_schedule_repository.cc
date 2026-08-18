#include "voicelife/storage_memory/memory_schedule_repository.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

#include "voicelife/schedule/schedule_query_score.h"

namespace voicelife::storage_memory {
namespace {

using schedule::DateTime;
using schedule::OperationRecord;
using schedule::QueryOperationCommand;
using schedule::QueryScheduleCommand;
using schedule::Schedule;
using schedule::ScheduleException;
using schedule::ScheduleRule;
using schedule::ScheduleStatus;
using schedule::ScheduleStatusFilter;

bool MatchesStatus(ScheduleStatus status, ScheduleStatusFilter filter) {
    return filter == ScheduleStatusFilter::kAll ||
           (filter == ScheduleStatusFilter::kActive && status == ScheduleStatus::kActive) ||
           (filter == ScheduleStatusFilter::kCancelled && status == ScheduleStatus::kCancelled) ||
           (filter == ScheduleStatusFilter::kCompleted && status == ScheduleStatus::kCompleted);
}

bool MatchesKeyword(std::string_view event, std::string_view keyword) {
    std::string normalized_event(event);
    std::string normalized_keyword(keyword);
    std::transform(normalized_event.begin(), normalized_event.end(), normalized_event.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    std::transform(normalized_keyword.begin(), normalized_keyword.end(), normalized_keyword.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    std::istringstream tokens(normalized_keyword);
    for (std::string token; tokens >> token;) {
        if (!token.empty() && token.front() == '+') token.erase(0, 1);
        if (!token.empty() && normalized_event.find(token) == std::string::npos) return false;
    }
    return true;
}

bool MatchesSchedule(const Schedule& schedule, const QueryScheduleCommand& query) {
    if (query.schedule_id.has_value() && schedule.id != *query.schedule_id) return false;
    if (query.rule_id.has_value() && (!schedule.rule_id.has_value() || *schedule.rule_id != *query.rule_id))
        return false;
    if (!MatchesStatus(schedule.status, query.status)) return false;
    if (query.keyword.has_value() && !MatchesKeyword(schedule.event, *query.keyword)) return false;
    if (query.start_from.has_value() || query.start_to.has_value()) {
        if (!schedule.start_time.has_value()) return false;
        if (query.start_from.has_value() && *schedule.start_time < *query.start_from) return false;
        if (query.start_to.has_value() && *schedule.start_time > *query.start_to) return false;
    }
    return true;
}

bool MatchesOperation(const OperationRecord& operation, const QueryOperationCommand& query) {
    if (query.operation_id.has_value() && operation.id != *query.operation_id) return false;
    if (query.entity_type.has_value() && operation.entity_type != *query.entity_type) return false;
    if (query.entity_id.has_value() && operation.entity_id != *query.entity_id) return false;
    if (query.type.has_value() && operation.type != *query.type) return false;
    if (query.operated_from.has_value() && operation.operated_at < *query.operated_from) return false;
    if (query.keyword.has_value() && !MatchesKeyword(operation.label, *query.keyword)) return false;
    return !query.operated_to.has_value() || operation.operated_at <= *query.operated_to;
}

template <typename Entity, typename Id>
auto FindEntityById(std::vector<Entity>& entities, Id id) {
    return std::find_if(entities.begin(), entities.end(), [id](const Entity& entity) { return entity.id == id; });
}

template <typename Entity, typename Id>
auto FindEntityById(const std::vector<Entity>& entities, Id id) {
    return std::find_if(entities.begin(), entities.end(), [id](const Entity& entity) { return entity.id == id; });
}

}  // namespace

DateTime MemoryScheduleRepository::Now() {
    return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
}

Result<Schedule> MemoryScheduleRepository::InsertScheduleLocked(const Schedule& input) {
    if (input.event.empty()) return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "日程名称不能为空");
    Schedule stored = input;
    stored.id = next_schedule_id_++;
    const DateTime now = Now();
    if (stored.created_at == DateTime{}) stored.created_at = now;
    if (stored.updated_at == DateTime{}) stored.updated_at = stored.created_at;
    schedules_.push_back(stored);
    return Result<Schedule>::Success(std::move(stored));
}

Result<Schedule> MemoryScheduleRepository::Insert(const Schedule& schedule) {
    std::lock_guard<std::mutex> lock(mutex_);
    return InsertScheduleLocked(schedule);
}

Status MemoryScheduleRepository::Update(const Schedule& schedule) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = FindEntityById(schedules_, schedule.id);
    if (found == schedules_.end()) return Status::Error(ErrorCode::kNotFound, "日程不存在");
    *found = schedule;
    return Status::Ok();
}

Status MemoryScheduleRepository::Delete(schedule::ScheduleId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = FindEntityById(schedules_, id);
    if (found == schedules_.end()) return Status::Error(ErrorCode::kNotFound, "日程不存在");
    if (found->status == ScheduleStatus::kCancelled)
        return Status::Error(ErrorCode::kConflict, "日程已取消，不能重复删除");
    found->status = ScheduleStatus::kCancelled;
    found->updated_at = Now();
    return Status::Ok();
}

Result<Schedule> MemoryScheduleRepository::FindById(schedule::ScheduleId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = FindEntityById(schedules_, id);
    if (found == schedules_.end()) return Result<Schedule>::Failure(ErrorCode::kNotFound, "未找到指定日程");
    return Result<Schedule>::Success(*found);
}

Result<std::vector<Schedule>> MemoryScheduleRepository::Find(const QueryScheduleCommand& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Schedule> result;
    for (const Schedule& schedule : schedules_) {
        if (MatchesSchedule(schedule, query)) result.push_back(schedule);
    }
    std::sort(result.begin(), result.end(), [&query](const Schedule& left, const Schedule& right) {
        if (query.keyword.has_value() && !query.keyword->empty()) {
            const int64_t left_score = schedule::ScoreScheduleKeyword(left.event, *query.keyword);
            const int64_t right_score = schedule::ScoreScheduleKeyword(right.event, *query.keyword);
            if (left_score != right_score) return left_score > right_score;
        }
        if (left.start_time != right.start_time) {
            if (!left.start_time.has_value()) return false;
            if (!right.start_time.has_value()) return true;
            return *left.start_time < *right.start_time;
        }
        return left.id < right.id;
    });
    const auto begin = std::min(static_cast<std::size_t>(query.offset), result.size());
    const auto count = std::min(static_cast<std::size_t>(query.limit), result.size() - begin);
    return Result<std::vector<Schedule>>::Success(
        std::vector<Schedule>(result.begin() + static_cast<std::ptrdiff_t>(begin),
                              result.begin() + static_cast<std::ptrdiff_t>(begin + count)));
}

Result<int64_t> MemoryScheduleRepository::Count(const QueryScheduleCommand& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Result<int64_t>::Success(
        static_cast<int64_t>(std::count_if(schedules_.begin(), schedules_.end(), [&query](const Schedule& schedule) {
            return MatchesSchedule(schedule, query);
        })));
}

Result<std::vector<Schedule>> MemoryScheduleRepository::FindOverlapping(
    DateTime start, DateTime end, std::optional<schedule::ScheduleId> exclude_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Schedule> result;
    for (const Schedule& schedule : schedules_) {
        if (schedule.status != ScheduleStatus::kActive || !schedule.start_time.has_value()) continue;
        if (exclude_id.has_value() && schedule.id == *exclude_id) continue;
        if (*schedule.start_time <= end && schedule.end_time.value_or(*schedule.start_time) >= start)
            result.push_back(schedule);
    }
    std::sort(result.begin(), result.end(),
              [](const Schedule& left, const Schedule& right) { return *left.start_time < *right.start_time; });
    return Result<std::vector<Schedule>>::Success(std::move(result));
}

Result<std::vector<Schedule>> MemoryScheduleRepository::FindAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Result<std::vector<Schedule>>::Success(schedules_);
}

Result<OperationRecord> MemoryScheduleRepository::InsertOperation(const OperationRecord& operation) {
    std::lock_guard<std::mutex> lock(mutex_);
    OperationRecord stored = operation;
    stored.id = next_operation_id_++;
    stored.operated_at = Now();
    operations_.push_back(stored);
    return Result<OperationRecord>::Success(std::move(stored));
}

Result<std::vector<OperationRecord>> MemoryScheduleRepository::FindOperations(
    const QueryOperationCommand& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OperationRecord> result;
    for (const OperationRecord& operation : operations_) {
        if (MatchesOperation(operation, query)) result.push_back(operation);
    }
    std::sort(result.begin(), result.end(), [](const OperationRecord& left, const OperationRecord& right) {
        return left.operated_at == right.operated_at ? left.id > right.id : left.operated_at > right.operated_at;
    });
    const auto begin = std::min(static_cast<std::size_t>(query.offset), result.size());
    const auto count = std::min(static_cast<std::size_t>(query.limit), result.size() - begin);
    return Result<std::vector<OperationRecord>>::Success(
        std::vector<OperationRecord>(result.begin() + static_cast<std::ptrdiff_t>(begin),
                                     result.begin() + static_cast<std::ptrdiff_t>(begin + count)));
}

Result<int64_t> MemoryScheduleRepository::CountOperations(const QueryOperationCommand& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Result<int64_t>::Success(static_cast<int64_t>(
        std::count_if(operations_.begin(), operations_.end(),
                      [&query](const OperationRecord& operation) { return MatchesOperation(operation, query); })));
}

Result<ScheduleRule> MemoryScheduleRuleRepository::InsertRuleLocked(const ScheduleRule& input) {
    if (input.event.empty()) return Result<ScheduleRule>::Failure(ErrorCode::kInvalidArgument, "规则名称不能为空");
    ScheduleRule stored = input;
    stored.id = repository_.next_rule_id_++;
    const DateTime now = MemoryScheduleRepository::Now();
    if (stored.created_at == DateTime{}) stored.created_at = now;
    if (stored.updated_at == DateTime{}) stored.updated_at = stored.created_at;
    repository_.rules_.push_back(stored);
    return Result<ScheduleRule>::Success(std::move(stored));
}

Result<ScheduleRule> MemoryScheduleRuleRepository::Insert(const ScheduleRule& rule) {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    return InsertRuleLocked(rule);
}

Status MemoryScheduleRuleRepository::Update(const ScheduleRule& rule) {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    const auto found = FindEntityById(repository_.rules_, rule.id);
    if (found == repository_.rules_.end()) return Status::Error(ErrorCode::kNotFound, "规则不存在");
    if (rule.event.empty()) return Status::Error(ErrorCode::kInvalidArgument, "规则名称不能为空");
    *found = rule;
    return Status::Ok();
}

Result<std::vector<ScheduleRule>> MemoryScheduleRuleRepository::FindAll() const {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    return Result<std::vector<ScheduleRule>>::Success(repository_.rules_);
}

Result<ScheduleRule> MemoryScheduleRuleRepository::FindById(schedule::ScheduleRuleId id) const {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    const auto found = FindEntityById(repository_.rules_, id);
    if (found == repository_.rules_.end()) return Result<ScheduleRule>::Failure(ErrorCode::kNotFound, "规则不存在");
    return Result<ScheduleRule>::Success(*found);
}

Result<ScheduleRule> MemoryScheduleRuleRepository::CreateWithFirstInstance(
    const ScheduleRule& rule, const std::optional<Schedule>& first_instance) {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    if (rule.event.empty() || (first_instance.has_value() && first_instance->event.empty())) {
        return Result<ScheduleRule>::Failure(ErrorCode::kInvalidArgument, "规则或首条日程名称不能为空");
    }
    const Result<ScheduleRule> created = InsertRuleLocked(rule);
    if (!created.ok()) return created;
    if (first_instance.has_value()) {
        Schedule instance = *first_instance;
        instance.rule_id = created.value->id;
        const Result<Schedule> inserted = repository_.InsertScheduleLocked(instance);
        if (!inserted.ok()) return Result<ScheduleRule>::Failure(inserted.status.code, inserted.status.message);
    }
    return created;
}

Result<ScheduleRule> MemoryScheduleRuleRepository::UpdateAndRebuild(const ScheduleRule& rule,
                                                                    const std::optional<Schedule>& first_instance) {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    const auto found = FindEntityById(repository_.rules_, rule.id);
    if (found == repository_.rules_.end()) return Result<ScheduleRule>::Failure(ErrorCode::kNotFound, "规则不存在");
    if (rule.event.empty() || (first_instance.has_value() && first_instance->event.empty())) {
        return Result<ScheduleRule>::Failure(ErrorCode::kInvalidArgument, "规则或首条日程名称不能为空");
    }
    *found = rule;
    const DateTime now = MemoryScheduleRepository::Now();
    repository_.schedules_.erase(std::remove_if(repository_.schedules_.begin(), repository_.schedules_.end(),
                                                [&rule, now](const Schedule& schedule) {
                                                    return schedule.rule_id == rule.id &&
                                                           schedule.status == ScheduleStatus::kActive &&
                                                           schedule.start_time.has_value() &&
                                                           *schedule.start_time > now;
                                                }),
                                 repository_.schedules_.end());
    repository_.exceptions_.erase(std::remove_if(repository_.exceptions_.begin(), repository_.exceptions_.end(),
                                                 [&rule, now](const ScheduleException& exception) {
                                                     return exception.rule_id == rule.id &&
                                                            exception.original_start_time > now;
                                                 }),
                                  repository_.exceptions_.end());
    if (first_instance.has_value()) {
        Schedule instance = *first_instance;
        instance.rule_id = rule.id;
        const Result<Schedule> inserted = repository_.InsertScheduleLocked(instance);
        if (!inserted.ok()) return Result<ScheduleRule>::Failure(inserted.status.code, inserted.status.message);
    }
    return Result<ScheduleRule>::Success(*found);
}

Status MemoryScheduleRuleRepository::CancelRuleAndInstances(schedule::ScheduleRuleId id,
                                                            int64_t& cancelled_instance_count) {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    const auto found = FindEntityById(repository_.rules_, id);
    if (found == repository_.rules_.end()) return Status::Error(ErrorCode::kNotFound, "规则不存在");
    found->status = ScheduleStatus::kCancelled;
    found->updated_at = MemoryScheduleRepository::Now();
    cancelled_instance_count = 0;
    for (Schedule& schedule : repository_.schedules_) {
        if (schedule.rule_id == id && schedule.status == ScheduleStatus::kActive) {
            schedule.status = ScheduleStatus::kCancelled;
            schedule.updated_at = found->updated_at;
            ++cancelled_instance_count;
        }
    }
    repository_.exceptions_.erase(
        std::remove_if(repository_.exceptions_.begin(), repository_.exceptions_.end(),
                       [id](const ScheduleException& exception) { return exception.rule_id == id; }),
        repository_.exceptions_.end());
    return Status::Ok();
}

Result<Schedule> MemoryScheduleRuleRepository::CreateNextInstance(
    const Schedule& schedule, const std::optional<ScheduleException>& linked_exception) {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    if (schedule.event.empty() || !schedule.rule_id.has_value() || *schedule.rule_id <= 0) {
        return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "日程实例字段无效");
    }
    const Result<Schedule> inserted = repository_.InsertScheduleLocked(schedule);
    if (!inserted.ok()) return inserted;
    if (linked_exception.has_value()) {
        ScheduleException linked = *linked_exception;
        linked.schedule_id = inserted.value->id;
        const Result<ScheduleException> saved = UpsertExceptionLocked(linked);
        if (!saved.ok()) return Result<Schedule>::Failure(saved.status.code, saved.status.message);
    }
    return inserted;
}

Result<ScheduleException> MemoryScheduleRuleRepository::UpsertExceptionLocked(const ScheduleException& exception) {
    if (exception.rule_id <= 0)
        return Result<ScheduleException>::Failure(ErrorCode::kInvalidArgument, "例外规则标识无效");
    const auto found = std::find_if(repository_.exceptions_.begin(), repository_.exceptions_.end(),
                                    [&exception](const ScheduleException& existing) {
                                        return existing.rule_id == exception.rule_id &&
                                               existing.original_start_time == exception.original_start_time;
                                    });
    ScheduleException stored = exception;
    const DateTime now = MemoryScheduleRepository::Now();
    if (found != repository_.exceptions_.end()) {
        stored.id = found->id;
        if (stored.created_at == DateTime{}) stored.created_at = found->created_at;
        if (stored.updated_at == DateTime{}) stored.updated_at = now;
        *found = stored;
    } else {
        stored.id = repository_.next_exception_id_++;
        if (stored.created_at == DateTime{}) stored.created_at = now;
        if (stored.updated_at == DateTime{}) stored.updated_at = stored.created_at;
        repository_.exceptions_.push_back(stored);
    }
    return Result<ScheduleException>::Success(std::move(stored));
}

Result<ScheduleException> MemoryScheduleRuleRepository::Upsert(const ScheduleException& exception) {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    return UpsertExceptionLocked(exception);
}

Result<std::vector<ScheduleException>> MemoryScheduleRuleRepository::FindByRule(
    schedule::ScheduleRuleId rule_id) const {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    std::vector<ScheduleException> result;
    for (const ScheduleException& exception : repository_.exceptions_) {
        if (exception.rule_id == rule_id) result.push_back(exception);
    }
    return Result<std::vector<ScheduleException>>::Success(std::move(result));
}

Result<std::optional<ScheduleException>> MemoryScheduleRuleRepository::FindByRuleAndTime(
    schedule::ScheduleRuleId rule_id, DateTime original_start_time) const {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    const auto found =
        std::find_if(repository_.exceptions_.begin(), repository_.exceptions_.end(),
                     [rule_id, original_start_time](const ScheduleException& exception) {
                         return exception.rule_id == rule_id && exception.original_start_time == original_start_time;
                     });
    return Result<std::optional<ScheduleException>>::Success(
        found == repository_.exceptions_.end() ? std::nullopt : std::optional<ScheduleException>(*found));
}

Status MemoryScheduleRuleRepository::DeleteFuture(schedule::ScheduleRuleId rule_id, DateTime after) {
    std::lock_guard<std::mutex> lock(repository_.mutex_);
    repository_.exceptions_.erase(std::remove_if(repository_.exceptions_.begin(), repository_.exceptions_.end(),
                                                 [rule_id, after](const ScheduleException& exception) {
                                                     return exception.rule_id == rule_id &&
                                                            exception.original_start_time > after;
                                                 }),
                                  repository_.exceptions_.end());
    return Status::Ok();
}

}  // namespace voicelife::storage_memory
