#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "support/in_memory_schedule_repository_helpers.h"
#include "support/schedule_repository_test_data.h"
#include "voicelife/schedule/schedule_operation_repository.h"
#include "voicelife/schedule/schedule_query_score.h"
#include "voicelife/schedule/schedule_repository.h"

namespace voicelife::test {

/**
 * @brief 为日程主机测试提供实例隔离的内存仓储。
 *
 * 该类型同时实现日程实体和操作记录仓储，仅供测试使用。每个实例独立保存
 * 数据，并在同一互斥区内完成操作记录的写入与查询。
 */
class InMemoryScheduleRepository final : public schedule::ScheduleRepository,
                                         public schedule::ScheduleOperationRepository {
   public:
    /**
     * @brief 使用指定日程集合创建空操作记录仓储。
     * @param schedules 初始日程集合。
     */
    explicit InMemoryScheduleRepository(std::vector<schedule::Schedule> schedules = {})
        : schedules_(std::move(schedules)),
          next_schedule_id_(in_memory_schedule_repository_helpers::NextScheduleId(schedules_)) {}

    /**
     * @brief 返回创建、修改和删除服务测试使用的固定日程。
     * @return 与原日程模拟数据等价的独立集合。
     */
    static std::vector<schedule::Schedule> DefaultSchedules() {
        return schedule_repository_test_data::DefaultSchedules();
    }

    /**
     * @brief 返回查询服务测试使用的固定日程。
     * @return 与原查询模拟数据等价的独立集合。
     */
    static std::vector<schedule::Schedule> QuerySchedules() { return schedule_repository_test_data::QuerySchedules(); }

    /**
     * @brief 插入日程并生成标识和缺失的时间戳。
     * @param input 待插入日程。
     * @return 保存后的完整日程。
     */
    Result<schedule::Schedule> Insert(const schedule::Schedule& input) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (input.event.empty()) {
            return Result<schedule::Schedule>::Failure(ErrorCode::kInvalidArgument, "日程名称不能为空");
        }
        schedule::Schedule stored = input;
        stored.id = next_schedule_id_++;
        const schedule::DateTime now = Now();
        if (stored.created_at == schedule::DateTime{}) stored.created_at = now;
        if (stored.updated_at == schedule::DateTime{}) stored.updated_at = stored.created_at;
        schedules_.push_back(stored);
        return Result<schedule::Schedule>::Success(std::move(stored));
    }

    /**
     * @brief 覆盖已有日程的全部字段。
     * @param input 待更新日程。
     * @return 找到并更新时返回成功。
     */
    Status Update(const schedule::Schedule& input) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_update_failure_.has_value()) {
            Status failure = std::move(*next_update_failure_);
            next_update_failure_.reset();
            return failure;
        }
        schedule::Schedule* stored = FindScheduleLocked(input.id);
        if (stored == nullptr) return Status::Error(ErrorCode::kNotFound, "日程不存在");
        *stored = input;
        return Status::Ok();
    }

    /**
     * @brief 将指定日程标记为已取消。
     * @param id 待取消日程标识。
     * @return 首次取消时返回成功。
     */
    Status Delete(schedule::ScheduleId id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        schedule::Schedule* stored = FindScheduleLocked(id);
        if (stored == nullptr) return Status::Error(ErrorCode::kNotFound, "日程不存在");
        if (stored->status == schedule::ScheduleStatus::kCancelled) {
            return Status::Error(ErrorCode::kConflict, "日程已取消，不能重复删除");
        }
        stored->status = schedule::ScheduleStatus::kCancelled;
        stored->updated_at = Now();
        return Status::Ok();
    }

    /**
     * @brief 返回当前实例中的全部日程。
     * @return 日程集合副本。
     */
    [[nodiscard]] Result<std::vector<schedule::Schedule>> FindAll() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_find_all_failure_.has_value()) {
            Status failure = std::move(*next_find_all_failure_);
            next_find_all_failure_.reset();
            return Result<std::vector<schedule::Schedule>>::Failure(failure.code, failure.message);
        }
        return Result<std::vector<schedule::Schedule>>::Success(schedules_);
    }

    [[nodiscard]] Result<schedule::Schedule> FindById(schedule::ScheduleId id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_find_by_id_failure_.has_value()) {
            Status failure = std::move(*next_find_by_id_failure_);
            next_find_by_id_failure_.reset();
            return Result<schedule::Schedule>::Failure(failure.code, failure.message);
        }
        const auto found = std::find_if(schedules_.begin(), schedules_.end(),
                                        [id](const schedule::Schedule& stored) { return stored.id == id; });
        if (found == schedules_.end())
            return Result<schedule::Schedule>::Failure(ErrorCode::kNotFound, "未找到指定日程");
        return Result<schedule::Schedule>::Success(*found);
    }

    [[nodiscard]] Result<std::vector<schedule::Schedule>> Find(
        const schedule::QueryScheduleCommand& query) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_find_failure_.has_value()) {
            Status failure = std::move(*next_find_failure_);
            next_find_failure_.reset();
            return Result<std::vector<schedule::Schedule>>::Failure(failure.code, failure.message);
        }
        std::vector<schedule::Schedule> matched;
        for (const schedule::Schedule& schedule : schedules_) {
            if (!in_memory_schedule_repository_helpers::MatchesQuery(schedule, query)) continue;
            matched.push_back(schedule);
        }
        std::sort(matched.begin(), matched.end(),
                  [&query](const schedule::Schedule& left, const schedule::Schedule& right) {
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
        const auto begin = std::min(static_cast<std::size_t>(query.offset), matched.size());
        const auto count = std::min(static_cast<std::size_t>(query.limit), matched.size() - begin);
        return Result<std::vector<schedule::Schedule>>::Success(
            std::vector<schedule::Schedule>(matched.begin() + static_cast<std::ptrdiff_t>(begin),
                                            matched.begin() + static_cast<std::ptrdiff_t>(begin + count)));
    }

    [[nodiscard]] Result<int64_t> Count(const schedule::QueryScheduleCommand& query) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_count_failure_.has_value()) {
            Status failure = std::move(*next_count_failure_);
            next_count_failure_.reset();
            return Result<int64_t>::Failure(failure.code, failure.message);
        }
        int64_t total = 0;
        for (const schedule::Schedule& schedule : schedules_) {
            if (in_memory_schedule_repository_helpers::MatchesQuery(schedule, query)) ++total;
        }
        return Result<int64_t>::Success(total);
    }

    [[nodiscard]] Result<std::vector<schedule::Schedule>> FindOverlapping(
        schedule::DateTime start, schedule::DateTime end,
        std::optional<schedule::ScheduleId> exclude_id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_find_overlapping_failure_.has_value()) {
            Status failure = std::move(*next_find_overlapping_failure_);
            next_find_overlapping_failure_.reset();
            return Result<std::vector<schedule::Schedule>>::Failure(failure.code, failure.message);
        }
        std::vector<schedule::Schedule> matched;
        for (const schedule::Schedule& schedule : schedules_) {
            if (schedule.status != schedule::ScheduleStatus::kActive || !schedule.start_time.has_value()) continue;
            if (exclude_id.has_value() && schedule.id == *exclude_id) continue;
            const schedule::DateTime schedule_start = *schedule.start_time;
            const schedule::DateTime schedule_end = schedule.end_time.value_or(schedule_start);
            if (schedule_start <= end && schedule_end >= start) matched.push_back(schedule);
        }
        std::sort(matched.begin(), matched.end(), [](const schedule::Schedule& left, const schedule::Schedule& right) {
            return *left.start_time < *right.start_time;
        });
        return Result<std::vector<schedule::Schedule>>::Success(std::move(matched));
    }

    /**
     * @brief 插入操作记录并生成标识和当前时间。
     * @param input 待插入操作记录。
     * @return 保存后的完整操作记录。
     */
    Result<schedule::OperationRecord> InsertOperation(const schedule::OperationRecord& input) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_insert_operation_failure_.has_value()) {
            Status failure = std::move(*next_insert_operation_failure_);
            next_insert_operation_failure_.reset();
            return Result<schedule::OperationRecord>::Failure(failure.code, failure.message);
        }
        return Result<schedule::OperationRecord>::Success(AppendOperationLocked(input, Now()));
    }

    /**
     * @brief 按筛选条件查询操作记录，按时间和标识倒序排列。
     * @param query 查询筛选和分页条件。
     * @return 匹配的操作记录，失败时返回错误状态。
     */
    [[nodiscard]] Result<std::vector<schedule::OperationRecord>> FindOperations(
        const schedule::QueryOperationCommand& query) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_find_operations_failure_.has_value()) {
            Status failure = std::move(*next_find_operations_failure_);
            next_find_operations_failure_.reset();
            return Result<std::vector<schedule::OperationRecord>>::Failure(failure.code, failure.message);
        }
        std::vector<schedule::OperationRecord> matched;
        for (const schedule::OperationRecord& operation : operations_) {
            if (in_memory_schedule_repository_helpers::MatchesOperation(operation, query)) matched.push_back(operation);
        }
        std::sort(matched.begin(), matched.end(),
                  [](const schedule::OperationRecord& left, const schedule::OperationRecord& right) {
                      if (left.operated_at != right.operated_at) return left.operated_at > right.operated_at;
                      return left.id > right.id;
                  });
        const auto begin = std::min(static_cast<std::size_t>(query.offset), matched.size());
        const auto count = std::min(static_cast<std::size_t>(query.limit), matched.size() - begin);
        return Result<std::vector<schedule::OperationRecord>>::Success(
            std::vector<schedule::OperationRecord>(matched.begin() + static_cast<std::ptrdiff_t>(begin),
                                                   matched.begin() + static_cast<std::ptrdiff_t>(begin + count)));
    }

    /**
     * @brief 统计满足筛选条件的操作总条数，不受分页影响。
     * @param query 查询筛选条件。
     * @return 操作总条数，失败时返回错误状态。
     */
    [[nodiscard]] Result<int64_t> CountOperations(const schedule::QueryOperationCommand& query) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_count_operations_failure_.has_value()) {
            Status failure = std::move(*next_count_operations_failure_);
            next_count_operations_failure_.reset();
            return Result<int64_t>::Failure(failure.code, failure.message);
        }
        int64_t total = 0;
        for (const schedule::OperationRecord& operation : operations_) {
            if (in_memory_schedule_repository_helpers::MatchesOperation(operation, query)) ++total;
        }
        return Result<int64_t>::Success(total);
    }

    /**
     * @brief 使用指定日程重置当前实例的全部测试状态。
     * @param schedules 新的日程集合。
     * @return 无。
     */
    void Reset(std::vector<schedule::Schedule> schedules = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        schedules_ = std::move(schedules);
        operations_.clear();
        next_schedule_id_ = in_memory_schedule_repository_helpers::NextScheduleId(schedules_);
        next_operation_id_ = 1;
        next_find_all_failure_.reset();
        next_find_by_id_failure_.reset();
        next_update_failure_.reset();
        next_find_overlapping_failure_.reset();
        next_find_failure_.reset();
        next_count_failure_.reset();
        next_insert_operation_failure_.reset();
        next_find_operations_failure_.reset();
        next_count_operations_failure_.reset();
    }

    /**
     * @brief 按指定时间插入操作记录，供时间窗口测试使用。
     * @param operation 待插入操作记录。
     * @param operated_at 指定操作时间。
     * @return 保存后的完整操作记录。
     */
    Result<schedule::OperationRecord> InsertOperationAt(const schedule::OperationRecord& operation,
                                                        schedule::DateTime operated_at) {
        std::lock_guard<std::mutex> lock(mutex_);
        return Result<schedule::OperationRecord>::Success(AppendOperationLocked(operation, operated_at));
    }

    /**
     * @brief 按标识读取日程测试数据。
     * @param id 日程标识。
     * @return 找到时返回日程副本。
     */
    [[nodiscard]] Result<schedule::Schedule> FindSchedule(schedule::ScheduleId id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const schedule::Schedule& stored : schedules_) {
            if (stored.id == id) return Result<schedule::Schedule>::Success(stored);
        }
        return Result<schedule::Schedule>::Failure(ErrorCode::kNotFound, "未找到指定日程");
    }

    /**
     * @brief 返回当前实例保存的全部操作记录。
     * @return 按写入顺序排列的操作记录副本。
     */
    [[nodiscard]] std::vector<schedule::OperationRecord> Operations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return operations_;
    }

    /**
     * @brief 注入下一次 FindOverlapping 失败状态。
     * @param status 下一次 FindOverlapping 返回的错误。
     * @return 无。
     */
    void FailNextFindOverlapping(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_find_overlapping_failure_ = std::move(status);
    }

    /**
     * @brief 注入下一次 FindAll 失败状态。
     * @param status 下一次 FindAll 返回的错误。
     * @return 无。
     */
    void FailNextFindAll(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_find_all_failure_ = std::move(status);
    }

    /**
     * @brief 注入下一次 FindById 失败状态。
     * @param status 下一次 FindById 返回的错误。
     * @return 无。
     */
    void FailNextFindById(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_find_by_id_failure_ = std::move(status);
    }

    /**
     * @brief 注入下一次 Update 失败状态。
     * @param status 下一次 Update 返回的错误。
     * @return 无。
     */
    void FailNextUpdate(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_update_failure_ = std::move(status);
    }

    /**
     * @brief 注入下一次 Find 失败状态。
     * @param status 下一次 Find 返回的错误。
     * @return 无。
     */
    void FailNextFind(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_find_failure_ = std::move(status);
    }

    /**
     * @brief 注入下一次 Count 失败状态。
     * @param status 下一次 Count 返回的错误。
     * @return 无。
     */
    void FailNextCount(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_count_failure_ = std::move(status);
    }

    /**
     * @brief 注入下一次 InsertOperation 失败状态。
     * @param status 下一次 InsertOperation 返回的错误。
     * @return 无。
     */
    void FailNextInsertOperation(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_insert_operation_failure_ = std::move(status);
    }

    /**
     * @brief 注入下一次 FindOperations 失败状态。
     * @param status 下一次 FindOperations 返回的错误。
     * @return 无。
     */
    void FailNextFindOperations(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_find_operations_failure_ = std::move(status);
    }

    /**
     * @brief 注入下一次 CountOperations 失败状态。
     * @param status 下一次 CountOperations 返回的错误。
     * @return 无。
     */
    void FailNextCountOperations(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_count_operations_failure_ = std::move(status);
    }

   private:
    using ScheduleIterator = std::vector<schedule::Schedule>::iterator;

    /** @brief 返回当前秒级系统时间。 @return 当前日程时间。 */
    static schedule::DateTime Now() {
        return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    }

    /** @brief 在锁内按标识查找日程。 @param id 日程标识。 @return 日程地址或 nullptr。 */
    schedule::Schedule* FindScheduleLocked(schedule::ScheduleId id) {
        const auto found = FindScheduleIteratorLocked(id);
        return found == schedules_.end() ? nullptr : &*found;
    }

    /** @brief 在锁内按标识查找日程迭代器。 @param id 日程标识。 @return 日程迭代器。 */
    ScheduleIterator FindScheduleIteratorLocked(schedule::ScheduleId id) {
        return std::find_if(schedules_.begin(), schedules_.end(),
                            [id](const schedule::Schedule& stored) { return stored.id == id; });
    }

    /**
     * @brief 在锁内追加操作记录。
     * @param input 操作数据。
     * @param operated_at 操作时间。
     * @return 保存后的完整操作记录。
     */
    schedule::OperationRecord AppendOperationLocked(const schedule::OperationRecord& input,
                                                    schedule::DateTime operated_at) {
        schedule::OperationRecord stored = input;
        stored.id = next_operation_id_++;
        stored.operated_at = operated_at;
        operations_.push_back(stored);
        return stored;
    }

    mutable std::mutex mutex_;
    std::vector<schedule::Schedule> schedules_;
    std::vector<schedule::OperationRecord> operations_;
    schedule::ScheduleId next_schedule_id_ = 1;
    schedule::OperationId next_operation_id_ = 1;
    mutable std::optional<Status> next_find_all_failure_;
    mutable std::optional<Status> next_find_by_id_failure_;
    std::optional<Status> next_update_failure_;
    mutable std::optional<Status> next_find_overlapping_failure_;
    mutable std::optional<Status> next_find_failure_;
    mutable std::optional<Status> next_count_failure_;
    std::optional<Status> next_insert_operation_failure_;
    mutable std::optional<Status> next_find_operations_failure_;
    mutable std::optional<Status> next_count_operations_failure_;
};

}  // namespace voicelife::test
