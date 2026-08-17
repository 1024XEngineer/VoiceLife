#pragma once

#include <mutex>
#include <optional>
#include <vector>

#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_rule_repository.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite {

/**
 * @brief 使用 SQLite 持久化周期规则与单次例外的具体仓储。
 *
 * 同时实现 ScheduleRuleRepository 和 ScheduleExceptionRepository 接口，共享同一个数据库连接
 * 和仓储锁，以便在单一事务内完成「创建规则 + 物化首条实例」等跨表原子操作。
 */
class SqliteScheduleRuleRepository final : public schedule::ScheduleRuleRepository,
                                           public schedule::ScheduleExceptionRepository {
   public:
    /**
     * @brief 创建使用指定数据库连接的 SQLite 周期仓储。
     * @param database 已构造的数据库连接管理器；其生命周期必须长于仓储。
     */
    explicit SqliteScheduleRuleRepository(SqliteDatabase& database);

    /** @brief 初始化周期规则与例外表结构。 @return 建表成功时返回成功状态。 */
    [[nodiscard]] Status Initialize();

    /** @brief 插入一条周期规则。 @param rule 待插入规则。 @return 保存后的完整规则。 */
    Result<schedule::ScheduleRule> Insert(const schedule::ScheduleRule& rule) override;
    /** @brief 更新已有规则的全部持久化字段。 @param rule 包含有效 id 的规则。 @return 更新结果。 */
    Status Update(const schedule::ScheduleRule& rule) override;
    /** @brief 读取仓储中的全部规则。 @return 规则集合或数据库错误。 */
    [[nodiscard]] Result<std::vector<schedule::ScheduleRule>> FindAll() const override;
    /** @brief 按标识读取一条规则。 @param id 规则标识。 @return 规则或未找到错误。 */
    [[nodiscard]] Result<schedule::ScheduleRule> FindById(schedule::ScheduleRuleId id) const override;
    /** @brief 在同一事务中创建规则并物化首条实例。 @param rule 待创建规则。 @param first_instance 首条实例。 @return
     * 保存后的规则。 */
    Result<schedule::ScheduleRule> CreateWithFirstInstance(
        const schedule::ScheduleRule& rule, const std::optional<schedule::Schedule>& first_instance) override;
    /** @brief 在同一事务中更新规则并重建未来实例。 @param rule 更新后的规则。 @param first_instance 新首条实例。
     * @return 保存后的规则。 */
    Result<schedule::ScheduleRule> UpdateAndRebuild(const schedule::ScheduleRule& rule,
                                                    const std::optional<schedule::Schedule>& first_instance) override;
    /** @brief 取消规则及已物化实例。 @param id 规则标识。 @param cancelled_instance_count 被取消实例数量。 @return
     * 取消结果。 */
    Status CancelRuleAndInstances(schedule::ScheduleRuleId id, int64_t& cancelled_instance_count) override;

    /** @brief 插入下一条日程实例。 @param schedule 待插入实例。 @param linked_exception 关联例外。 @return
     * 保存后的实例。 */
    Result<schedule::Schedule> CreateNextInstance(
        const schedule::Schedule& schedule,
        const std::optional<schedule::ScheduleException>& linked_exception) override;

    /** @brief 插入或更新一条单次例外。 @param exception 待写入例外。 @return 保存后的例外。 */
    Result<schedule::ScheduleException> Upsert(const schedule::ScheduleException& exception) override;
    /** @brief 读取某规则的全部例外。 @param rule_id 规则标识。 @return 例外列表。 */
    [[nodiscard]] Result<std::vector<schedule::ScheduleException>> FindByRule(
        schedule::ScheduleRuleId rule_id) const override;
    /** @brief 按逻辑键读取一条例外。 @param rule_id 规则标识。 @param original_start_time 原始发生时间。 @return 例外。
     */
    [[nodiscard]] Result<std::optional<schedule::ScheduleException>> FindByRuleAndTime(
        schedule::ScheduleRuleId rule_id, schedule::DateTime original_start_time) const override;
    /** @brief 删除指定时间之后的未发生例外。 @param rule_id 规则标识。 @param after 删除边界。 @return 删除结果。 */
    Status DeleteFuture(schedule::ScheduleRuleId rule_id, schedule::DateTime after) override;

   private:
    /** @brief 在调用方持有仓储锁时插入规则。 @param rule 待插入规则。 @return 保存结果。 */
    Result<schedule::ScheduleRule> InsertRuleLocked(const schedule::ScheduleRule& rule);
    /** @brief 在调用方持有仓储锁时插入日程实例。 @param schedule 待插入实例。 @return 保存结果。 */
    Result<schedule::Schedule> InsertScheduleLocked(const schedule::Schedule& schedule);
    /** @brief 在调用方持有仓储锁时按逻辑键读取例外。 */
    Result<std::optional<schedule::ScheduleException>> FindByRuleAndTimeLocked(
        schedule::ScheduleRuleId rule_id, schedule::DateTime original_start_time) const;
    Result<schedule::ScheduleException> UpsertExceptionLocked(const schedule::ScheduleException& exception);

    SqliteDatabase& database_;
    mutable std::mutex mutex_;
};

}  // namespace voicelife::storage_sqlite
