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

    Result<schedule::ScheduleRule> Insert(const schedule::ScheduleRule& rule) override;
    Status Update(const schedule::ScheduleRule& rule) override;
    [[nodiscard]] Result<std::vector<schedule::ScheduleRule>> FindAll() const override;
    [[nodiscard]] Result<schedule::ScheduleRule> FindById(schedule::ScheduleRuleId id) const override;
    Result<schedule::ScheduleRule> CreateWithFirstInstance(
        const schedule::ScheduleRule& rule, const std::optional<schedule::Schedule>& first_instance) override;
    Result<schedule::ScheduleRule> UpdateAndRebuild(const schedule::ScheduleRule& rule,
                                                    const std::optional<schedule::Schedule>& first_instance) override;
    Status CancelRuleAndInstances(schedule::ScheduleRuleId id, int64_t& cancelled_instance_count) override;

    Result<schedule::Schedule> CreateNextInstance(
        const schedule::Schedule& schedule,
        const std::optional<schedule::ScheduleException>& linked_exception) override;

    Result<schedule::ScheduleException> Upsert(const schedule::ScheduleException& exception) override;
    [[nodiscard]] Result<std::vector<schedule::ScheduleException>> FindByRule(
        schedule::ScheduleRuleId rule_id) const override;
    [[nodiscard]] Result<std::optional<schedule::ScheduleException>> FindByRuleAndTime(
        schedule::ScheduleRuleId rule_id, schedule::DateTime original_start_time) const override;
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
