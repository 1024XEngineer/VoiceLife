#pragma once

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
 * 数据，并在同一互斥区内完成撤销所需的实体恢复、原操作失效和撤销记录写入。
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
        return Result<std::vector<schedule::Schedule>>::Success(schedules_);
    }

    [[nodiscard]] Result<schedule::Schedule> FindById(schedule::ScheduleId id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = std::find_if(schedules_.begin(), schedules_.end(),
                                        [id](const schedule::Schedule& stored) { return stored.id == id; });
        if (found == schedules_.end())
            return Result<schedule::Schedule>::Failure(ErrorCode::kNotFound, "未找到指定日程");
        return Result<schedule::Schedule>::Success(*found);
    }

    [[nodiscard]] Result<std::vector<schedule::Schedule>> Find(
        const schedule::QueryScheduleCommand& query) const override {
        std::lock_guard<std::mutex> lock(mutex_);
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
        return Result<schedule::OperationRecord>::Success(AppendOperationLocked(input, Now()));
    }

    /**
     * @brief 查询十五分钟闭区间内仍有效的操作记录。
     * @param now 查询窗口结束时间。
     * @return 按时间和标识倒序排列的操作记录。
     */
    [[nodiscard]] Result<std::vector<schedule::OperationRecord>> FindRecentOperations(
        schedule::DateTime now) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<schedule::OperationRecord> result;
        for (const StoredOperation& stored : operations_) {
            if (stored.active && in_memory_schedule_repository_helpers::IsWithinUndoWindow(stored.operation, now))
                result.push_back(stored.operation);
        }
        std::sort(result.begin(), result.end(),
                  [](const schedule::OperationRecord& left, const schedule::OperationRecord& right) {
                      if (left.operated_at != right.operated_at) return left.operated_at > right.operated_at;
                      return left.id > right.id;
                  });
        return Result<std::vector<schedule::OperationRecord>>::Success(std::move(result));
    }

    /**
     * @brief 原子撤销指定操作并追加撤销记录。
     * @param operation_id 待撤销操作标识。
     * @param now 撤销时间和窗口结束时间。
     * @return 原操作以及撤销完成后的日程。
     */
    Result<schedule::UndoOperationResult> UndoOperation(schedule::OperationId operation_id,
                                                        schedule::DateTime now) override {
        std::lock_guard<std::mutex> lock(mutex_);
        StoredOperation* target = FindOperationLocked(operation_id);
        const Result<schedule::OperationRecord> validated = ValidateUndoableOperation(target, now);
        if (!validated.ok()) {
            return Result<schedule::UndoOperationResult>::Failure(validated.status.code, validated.status.message);
        }
        if (next_undo_failure_.has_value()) {
            Status failure = std::move(*next_undo_failure_);
            next_undo_failure_.reset();
            return Result<schedule::UndoOperationResult>::Failure(failure.code, failure.message);
        }

        const schedule::OperationRecord original = *validated.value;
        const auto current = FindScheduleIteratorLocked(original.schedule_id);
        const std::optional<schedule::Schedule> before =
            current == schedules_.end() ? std::nullopt : std::optional<schedule::Schedule>{*current};
        std::optional<schedule::Schedule> after;
        const Status applied = ApplyUndoLocked(original, current, after);
        if (!applied.ok()) {
            return Result<schedule::UndoOperationResult>::Failure(applied.code, applied.message);
        }

        const std::string event =
            before.has_value() ? before->event : (after.has_value() ? after->event : original.schedule_event);
        const schedule::OperationRecord undo{
            .id = 0,
            .type = schedule::ScheduleOperationType::kUndo,
            .schedule_id = original.schedule_id,
            .schedule_event = event,
            .operated_at = {},
            .previous = before,
        };
        target->active = false;
        (void)AppendOperationLocked(undo, now);
        return Result<schedule::UndoOperationResult>::Success({.operation = original, .schedule = std::move(after)});
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
        next_undo_failure_.reset();
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
     * @brief 返回当前仍有效的全部操作记录。
     * @return 按写入顺序排列的操作记录副本。
     */
    [[nodiscard]] std::vector<schedule::OperationRecord> ActiveOperations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<schedule::OperationRecord> result;
        for (const StoredOperation& stored : operations_) {
            if (stored.active) result.push_back(stored.operation);
        }
        return result;
    }

    /**
     * @brief 校验指定操作在给定时间是否仍可撤销。
     * @param operation_id 操作标识。
     * @param now 校验时间。
     * @return 可撤销时返回操作记录。
     */
    [[nodiscard]] Result<schedule::OperationRecord> FindUndoableOperation(schedule::OperationId operation_id,
                                                                          schedule::DateTime now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ValidateUndoableOperation(FindOperationLocked(operation_id), now);
    }

    /**
     * @brief 注入下一次撤销失败状态。
     * @param status 下一次 UndoOperation 返回的错误。
     * @return 无。
     */
    void FailNextUndo(Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_undo_failure_ = std::move(status);
    }

   private:
    /** @brief 内存中的操作条目。 */
    struct StoredOperation {
        schedule::OperationRecord operation;
        bool active = true;
    };

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

    /** @brief 在锁内按标识查找操作。 @param id 操作标识。 @return 操作地址或 nullptr。 */
    StoredOperation* FindOperationLocked(schedule::OperationId id) {
        for (StoredOperation& stored : operations_) {
            if (stored.operation.id == id) return &stored;
        }
        return nullptr;
    }

    /** @brief 在锁内按标识查找操作。 @param id 操作标识。 @return 操作地址或 nullptr。 */
    const StoredOperation* FindOperationLocked(schedule::OperationId id) const {
        for (const StoredOperation& stored : operations_) {
            if (stored.operation.id == id) return &stored;
        }
        return nullptr;
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
        operations_.push_back({.operation = stored, .active = true});
        return stored;
    }

    /**
     * @brief 校验锁内找到的操作是否可撤销。
     * @param stored 操作条目。
     * @param now 当前时间。
     * @return 可撤销时返回操作记录。
     */
    static Result<schedule::OperationRecord> ValidateUndoableOperation(const StoredOperation* stored,
                                                                       schedule::DateTime now) {
        if (stored == nullptr || !stored->active) {
            return Result<schedule::OperationRecord>::Failure(ErrorCode::kNotFound, "操作不存在或已撤销");
        }
        if (stored->operation.operated_at > now) {
            return Result<schedule::OperationRecord>::Failure(ErrorCode::kConflict, "操作时间晚于当前时间，不能撤销");
        }
        if (!in_memory_schedule_repository_helpers::IsWithinUndoWindow(stored->operation, now)) {
            return Result<schedule::OperationRecord>::Failure(ErrorCode::kConflict, "操作已超过十五分钟撤销期限");
        }
        return Result<schedule::OperationRecord>::Success(stored->operation);
    }

    /**
     * @brief 在锁内应用日程逆操作。
     * @param operation 被撤销操作。
     * @param current 当前日程迭代器。
     * @param after 接收撤销后的日程。
     * @return 应用结果。
     */
    Status ApplyUndoLocked(const schedule::OperationRecord& operation, ScheduleIterator current,
                           std::optional<schedule::Schedule>& after) {
        if (operation.previous.has_value() && operation.previous->id != operation.schedule_id) {
            return Status::Error(ErrorCode::kInternal, "操作记录的日程快照与目标 ID 不一致");
        }
        switch (operation.type) {
            case schedule::ScheduleOperationType::kCreate:
                if (current == schedules_.end()) return Status::Error(ErrorCode::kNotFound, "未找到指定日程");
                schedules_.erase(current);
                return Status::Ok();
            case schedule::ScheduleOperationType::kUpdate:
                if (!operation.previous.has_value()) {
                    return Status::Error(ErrorCode::kInternal, "操作记录缺少可恢复的日程快照");
                }
                if (current == schedules_.end()) return Status::Error(ErrorCode::kNotFound, "未找到指定日程");
                *current = *operation.previous;
                after = operation.previous;
                return Status::Ok();
            case schedule::ScheduleOperationType::kDelete:
                if (!operation.previous.has_value()) {
                    return Status::Error(ErrorCode::kInternal, "操作记录缺少可恢复的日程快照");
                }
                if (current == schedules_.end())
                    schedules_.push_back(*operation.previous);
                else
                    *current = *operation.previous;
                after = operation.previous;
                return Status::Ok();
            case schedule::ScheduleOperationType::kUndo:
                if (operation.previous.has_value()) {
                    if (current == schedules_.end()) {
                        schedules_.push_back(*operation.previous);
                    } else {
                        *current = *operation.previous;
                    }
                    after = operation.previous;
                    return Status::Ok();
                }
                if (current == schedules_.end()) return Status::Error(ErrorCode::kNotFound, "未找到指定日程");
                schedules_.erase(current);
                return Status::Ok();
            default:
                return Status::Error(ErrorCode::kInternal, "操作记录包含不支持的类型");
        }
    }

    mutable std::mutex mutex_;
    std::vector<schedule::Schedule> schedules_;
    std::vector<StoredOperation> operations_;
    schedule::ScheduleId next_schedule_id_ = 1;
    schedule::OperationId next_operation_id_ = 1;
    std::optional<Status> next_undo_failure_;
};

}  // namespace voicelife::test
