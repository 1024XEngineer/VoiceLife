#pragma once

#include <mutex>
#include <optional>
#include <vector>

#include "voicelife/schedule/schedule_operation_repository.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite {

/**
 * @brief 使用 SQLite 持久化日程实体的具体仓储。
 *
 * SQL 文本和 SQLite 行映射位于实现目录，业务服务只能看到 ScheduleRepository 接口。
 */
class SqliteScheduleRepository final : public schedule::ScheduleRepository,
                                       public schedule::ScheduleOperationRepository {
   public:
    /**
     * @brief 创建使用指定数据库连接的 SQLite 日程仓储。
     * @param database 已构造的数据库连接管理器；其生命周期必须长于仓储。
     */
    explicit SqliteScheduleRepository(SqliteDatabase& database);

    /**
     * @brief 初始化日程表结构。
     * @return 建表成功时返回成功状态，否则返回数据库错误。
     */
    [[nodiscard]] Status Initialize();

    /**
     * @brief 插入一条日程。
     * @param schedule 待插入的日程。
     * @return 实际保存后的日程。
     */
    Result<schedule::Schedule> Insert(const schedule::Schedule& schedule) override;

    /** @brief 更新一条日程。 @param schedule 待更新日程。 @return 更新状态。 */
    Status Update(const schedule::Schedule& schedule) override;

    /** @brief 将一条日程标记为已取消。 @param id 日程标识。 @return 软取消状态。 */
    Status Delete(schedule::ScheduleId id) override;

    /**
     * @brief 读取全部日程。
     * @return 按开始时间和标识排序的日程集合。
     */
    [[nodiscard]] Result<std::vector<schedule::Schedule>> FindAll() const override;

    /** @brief 按标识读取一条日程。 @param id 日程标识。 @return 日程或未找到错误。 */
    [[nodiscard]] Result<schedule::Schedule> FindById(schedule::ScheduleId id) const override;

    /** @brief 按条件读取当前页日程。 @param query 查询条件。 @return 当前页日程集合。 */
    [[nodiscard]] Result<std::vector<schedule::Schedule>> Find(
        const schedule::QueryScheduleCommand& query) const override;

    /** @brief 按条件统计日程总数。 @param query 查询条件。 @return 总数。 */
    [[nodiscard]] Result<int64_t> Count(const schedule::QueryScheduleCommand& query) const override;

    /**
     * @brief 查询与时间窗口可能重叠的有效日程。
     * @param start 窗口起点。
     * @param end 窗口终点；单点日程应传同一时间。
     * @param exclude_id 排除的日程标识。
     * @return 可能重叠的有效日程集合。
     */
    [[nodiscard]] Result<std::vector<schedule::Schedule>> FindOverlapping(
        schedule::DateTime start, schedule::DateTime end,
        std::optional<schedule::ScheduleId> exclude_id) const override;

    /**
     * @brief 插入一条日程操作记录。
     * @param operation 待保存的操作。
     * @return 实际保存后的完整操作记录。
     */
    Result<schedule::OperationRecord> InsertOperation(const schedule::OperationRecord& operation) override;

    /**
     * @brief 查询十五分钟闭区间内仍有效的操作记录。
     * @param now 查询窗口结束时间。
     * @return 按时间和标识倒序排列的操作记录。
     */
    [[nodiscard]] Result<std::vector<schedule::OperationRecord>> FindRecentOperations(
        schedule::DateTime now) const override;

    /**
     * @brief 在单个立即事务内执行日程逆操作并写入撤销记录。
     * @param operation_id 要撤销的操作标识。
     * @param now 撤销发生时间。
     * @return 原操作及撤销后的日程。
     */
    Result<schedule::UndoOperationResult> UndoOperation(schedule::OperationId operation_id,
                                                        schedule::DateTime now) override;

   private:
    /** @brief 在调用方持有仓储锁时读取指定日程。 @param id 日程标识。 @return 日程或错误。 */
    Result<schedule::Schedule> FindByIdLocked(schedule::ScheduleId id) const;
    /** @brief 在调用方持有仓储锁时插入操作。 @param operation 待保存操作。 @return 保存结果。 */
    Result<schedule::OperationRecord> InsertOperationLocked(const schedule::OperationRecord& operation);
    /**
     * @brief 在调用方持有仓储锁时恢复日程快照。
     * @param snapshot 完整快照。
     * @param require_existing 是否要求目标已存在。
     * @return 恢复结果。
     */
    Status RestoreScheduleLocked(const schedule::Schedule& snapshot, bool require_existing);
    /** @brief 在调用方持有仓储锁时物理删除日程。 @param id 日程标识。 @return 删除状态。 */
    Status RemoveScheduleLocked(schedule::ScheduleId id);
    /** @brief 将撤销失败转换为事务回滚后的状态。 @param failure 原始失败状态。 @return 保留原始错误的状态。 */
    Status RollbackAfterFailure(const Status& failure);

    SqliteDatabase& database_;
    mutable std::mutex mutex_;
};

}  // namespace voicelife::storage_sqlite
