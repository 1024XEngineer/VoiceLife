#pragma once

#include <mutex>
#include <optional>
#include <vector>

#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_operation_repository.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_rule_repository.h"

namespace voicelife::storage_memory {

/**
 * @brief 进程内日程存储，供没有专用数据分区的设备 Profile 使用。
 *
 * 同一个互斥锁保护四类实体，跨表方法在锁内完成；数据在重启后不保留。
 */
class MemoryScheduleRuleRepository;

/** @brief 实现日程与操作记录的易失内存仓储。 */
class MemoryScheduleRepository final : public schedule::ScheduleRepository,
                                       public schedule::ScheduleOperationRepository {
   public:
    /** @brief 插入日程。 @param schedule 待保存日程。 @return 保存后的日程或错误。 */
    Result<schedule::Schedule> Insert(const schedule::Schedule& schedule) override;
    /** @brief 更新日程。 @param schedule 含有效标识的日程。 @return 更新状态。 */
    Status Update(const schedule::Schedule& schedule) override;
    /** @brief 取消日程。 @param id 日程标识。 @return 取消状态。 */
    Status Delete(schedule::ScheduleId id) override;
    /** @brief 读取日程。 @param id 日程标识。 @return 日程或错误。 */
    [[nodiscard]] Result<schedule::Schedule> FindById(schedule::ScheduleId id) const override;
    /** @brief 查询日程。 @param query 筛选与分页条件。 @return 命中的日程集合。 */
    [[nodiscard]] Result<std::vector<schedule::Schedule>> Find(
        const schedule::QueryScheduleCommand& query) const override;
    /** @brief 统计日程。 @param query 筛选条件。 @return 命中总数。 */
    [[nodiscard]] Result<int64_t> Count(const schedule::QueryScheduleCommand& query) const override;
    /** @brief 查询重叠日程。 @param start 窗口起点。 @param end 窗口终点。 @param exclude_id 排除的日程标识。 @return
     * 重叠日程集合。 */
    [[nodiscard]] Result<std::vector<schedule::Schedule>> FindOverlapping(
        schedule::DateTime start, schedule::DateTime end,
        std::optional<schedule::ScheduleId> exclude_id) const override;
    /** @brief 返回全部日程。 @return 日程集合。 */
    [[nodiscard]] Result<std::vector<schedule::Schedule>> FindAll() const override;

    /** @brief 写入操作记录。 @param operation 待保存记录。 @return 已盖章的记录。 */
    Result<schedule::OperationRecord> InsertOperation(const schedule::OperationRecord& operation) override;
    /** @brief 查询操作记录。 @param query 筛选与分页条件。 @return 操作记录集合。 */
    [[nodiscard]] Result<std::vector<schedule::OperationRecord>> FindOperations(
        const schedule::QueryOperationCommand& query) const override;
    /** @brief 统计操作记录。 @param query 筛选条件。 @return 命中总数。 */
    [[nodiscard]] Result<int64_t> CountOperations(const schedule::QueryOperationCommand& query) const override;

   private:
    friend class MemoryScheduleRuleRepository;
    static schedule::DateTime Now();
    Result<schedule::Schedule> InsertScheduleLocked(const schedule::Schedule& schedule);

    mutable std::mutex mutex_;
    std::vector<schedule::Schedule> schedules_;
    std::vector<schedule::OperationRecord> operations_;
    std::vector<schedule::ScheduleRule> rules_;
    std::vector<schedule::ScheduleException> exceptions_;
    schedule::ScheduleId next_schedule_id_ = 1;
    schedule::OperationId next_operation_id_ = 1;
    schedule::ScheduleRuleId next_rule_id_ = 1;
    schedule::ScheduleExceptionId next_exception_id_ = 1;
};

/** @brief 复用 MemoryScheduleRepository 的共享状态实现周期规则与例外接口。 */
class MemoryScheduleRuleRepository final : public schedule::ScheduleRuleRepository,
                                           public schedule::ScheduleExceptionRepository {
   public:
    /** @brief 绑定共享内存状态。 @param repository 对应的日程仓储。 */
    explicit MemoryScheduleRuleRepository(MemoryScheduleRepository& repository) : repository_(repository) {}

    /** @brief 插入周期规则。 @param rule 待保存规则。 @return 保存后的规则或错误。 */
    Result<schedule::ScheduleRule> Insert(const schedule::ScheduleRule& rule) override;
    /** @brief 更新周期规则。 @param rule 含有效标识的规则。 @return 更新状态。 */
    Status Update(const schedule::ScheduleRule& rule) override;
    /** @brief 返回全部周期规则。 @return 规则集合。 */
    [[nodiscard]] Result<std::vector<schedule::ScheduleRule>> FindAll() const override;
    /** @brief 读取周期规则。 @param id 规则标识。 @return 规则或错误。 */
    [[nodiscard]] Result<schedule::ScheduleRule> FindById(schedule::ScheduleRuleId id) const override;
    /** @brief 原子创建规则和首条实例。 @param rule 待保存规则。 @param first_instance 首条实例。 @return
     * 保存后的规则或错误。 */
    Result<schedule::ScheduleRule> CreateWithFirstInstance(
        const schedule::ScheduleRule& rule, const std::optional<schedule::Schedule>& first_instance) override;
    /** @brief 原子更新规则并重建未来实例。 @param rule 更新后的规则。 @param first_instance 新首条实例。 @return
     * 更新后的规则或错误。 */
    Result<schedule::ScheduleRule> UpdateAndRebuild(const schedule::ScheduleRule& rule,
                                                    const std::optional<schedule::Schedule>& first_instance) override;
    /** @brief 取消规则及实例。 @param id 规则标识。 @param cancelled_instance_count 输出取消实例数。 @return 取消状态。
     */
    Status CancelRuleAndInstances(schedule::ScheduleRuleId id, int64_t& cancelled_instance_count) override;
    /** @brief 原子创建下一实例。 @param schedule 待保存实例。 @param linked_exception 待回写例外。 @return
     * 保存后的实例或错误。 */
    Result<schedule::Schedule> CreateNextInstance(
        const schedule::Schedule& schedule,
        const std::optional<schedule::ScheduleException>& linked_exception) override;

    /** @brief 插入或更新例外。 @param exception 待保存例外。 @return 保存后的例外或错误。 */
    Result<schedule::ScheduleException> Upsert(const schedule::ScheduleException& exception) override;
    /** @brief 查询规则的例外。 @param rule_id 规则标识。 @return 例外集合。 */
    [[nodiscard]] Result<std::vector<schedule::ScheduleException>> FindByRule(
        schedule::ScheduleRuleId rule_id) const override;
    /** @brief 查询单个例外。 @param rule_id 规则标识。 @param original_start_time 原始发生时间。 @return 可空例外。 */
    [[nodiscard]] Result<std::optional<schedule::ScheduleException>> FindByRuleAndTime(
        schedule::ScheduleRuleId rule_id, schedule::DateTime original_start_time) const override;
    /** @brief 删除未来例外。 @param rule_id 规则标识。 @param after 时间边界。 @return 删除状态。 */
    Status DeleteFuture(schedule::ScheduleRuleId rule_id, schedule::DateTime after) override;

   private:
    Result<schedule::ScheduleRule> InsertRuleLocked(const schedule::ScheduleRule& rule);
    Result<schedule::ScheduleException> UpsertExceptionLocked(const schedule::ScheduleException& exception);

    MemoryScheduleRepository& repository_;
};

}  // namespace voicelife::storage_memory
