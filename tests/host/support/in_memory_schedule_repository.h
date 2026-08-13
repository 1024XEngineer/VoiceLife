#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "voicelife/schedule/schedule_operation_repository.h"
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
        : schedules_(std::move(schedules)), next_schedule_id_(NextScheduleId(schedules_)) {}

    /**
     * @brief 返回创建、修改和删除服务测试使用的固定日程。
     * @return 与原日程模拟数据等价的独立集合。
     */
    static std::vector<schedule::Schedule> DefaultSchedules() {
        return {
            schedule::Schedule{
                .id = 1001,
                .event = "模拟团队周会",
                .start_time = At(1'800'000'000),
                .end_time = At(1'800'003'600),
                .location = std::nullopt,
                .notes = std::nullopt,
                .rule_id = std::nullopt,
                .status = schedule::ScheduleStatus::kActive,
                .created_at = At(1'799'900'000),
                .updated_at = At(1'799'900'000),
            },
            schedule::Schedule{
                .id = 1002,
                .event = "模拟单点日程",
                .start_time = At(1'800'007'200),
                .end_time = std::nullopt,
                .location = std::nullopt,
                .notes = std::nullopt,
                .rule_id = std::nullopt,
                .status = schedule::ScheduleStatus::kActive,
                .created_at = At(1'799'900'000),
                .updated_at = At(1'799'900'000),
            },
            schedule::Schedule{
                .id = 1003,
                .event = "模拟周期规则实例",
                .start_time = At(1'800'010'800),
                .end_time = At(1'800'014'400),
                .location = std::nullopt,
                .notes = std::nullopt,
                .rule_id = 3001,
                .status = schedule::ScheduleStatus::kActive,
                .created_at = At(1'799'900'000),
                .updated_at = At(1'799'900'000),
            },
        };
    }

    /**
     * @brief 返回查询服务测试使用的固定日程。
     * @return 与原查询模拟数据等价的独立集合。
     */
    static std::vector<schedule::Schedule> QuerySchedules() {
        return {
            schedule::Schedule{
                .id = 2001,
                .event = "数据库连接评审",
                .start_time = At(1'810'000'000),
                .end_time = At(1'810'003'600),
                .location = "会议室 A",
                .notes = std::nullopt,
                .rule_id = std::nullopt,
                .status = schedule::ScheduleStatus::kActive,
                .created_at = At(1'809'900'000),
                .updated_at = At(1'809'900'000),
            },
            schedule::Schedule{
                .id = 2002,
                .event = "数据库连接复盘",
                .start_time = At(1'810'007'200),
                .end_time = std::nullopt,
                .location = "线上",
                .notes = std::nullopt,
                .rule_id = std::nullopt,
                .status = schedule::ScheduleStatus::kCompleted,
                .created_at = At(1'809'900'100),
                .updated_at = At(1'810'008'000),
            },
            schedule::Schedule{
                .id = 2003,
                .event = "产品方案讨论",
                .start_time = At(1'810'003'600),
                .end_time = At(1'810'005'400),
                .location = "会议室 B",
                .notes = std::nullopt,
                .rule_id = std::nullopt,
                .status = schedule::ScheduleStatus::kCancelled,
                .created_at = At(1'809'900'200),
                .updated_at = At(1'809'901'000),
            },
            schedule::Schedule{
                .id = 2004,
                .event = "整理周报",
                .start_time = std::nullopt,
                .end_time = std::nullopt,
                .location = std::nullopt,
                .notes = std::nullopt,
                .rule_id = std::nullopt,
                .status = schedule::ScheduleStatus::kActive,
                .created_at = At(1'809'900'300),
                .updated_at = At(1'809'900'300),
            },
        };
    }

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
            if (stored.active && IsWithinUndoWindow(stored.operation, now)) result.push_back(stored.operation);
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
        next_schedule_id_ = NextScheduleId(schedules_);
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

    /** @brief 转换 Unix 秒。 @param seconds Unix 秒。 @return 日程时间。 */
    static schedule::DateTime At(int64_t seconds) { return schedule::DateTime{std::chrono::seconds{seconds}}; }

    /**
     * @brief 计算下一条日程标识。
     * @param schedules 已有日程。
     * @return 大于全部已有标识的正整数。
     */
    static schedule::ScheduleId NextScheduleId(const std::vector<schedule::Schedule>& schedules) {
        schedule::ScheduleId next = 1;
        for (const schedule::Schedule& stored : schedules) next = std::max(next, stored.id + 1);
        return next;
    }

    /**
     * @brief 判断操作是否位于撤销窗口内。
     * @param operation 操作记录。
     * @param now 当前时间。
     * @return 操作时间位于闭区间时返回 true。
     */
    static bool IsWithinUndoWindow(const schedule::OperationRecord& operation, schedule::DateTime now) {
        return operation.operated_at >= now - std::chrono::minutes{15} && operation.operated_at <= now;
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
        if (!IsWithinUndoWindow(stored->operation, now)) {
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
