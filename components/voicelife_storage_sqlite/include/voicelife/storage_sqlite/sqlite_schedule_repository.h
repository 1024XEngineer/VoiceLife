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
     * @param operation 待保存的操作；仓储负责生成 id 和 operated_at。
     * @return 实际保存后的完整操作记录。
     */
    Result<schedule::OperationRecord> InsertOperation(const schedule::OperationRecord& operation) override;

    /**
     * @brief 按筛选条件查询操作记录，按 operated_at DESC, id DESC 排序。
     * @param query 查询筛选和分页条件。
     * @return 匹配的操作记录。
     */
    [[nodiscard]] Result<std::vector<schedule::OperationRecord>> FindOperations(
        const schedule::QueryOperationCommand& query) const override;

    /**
     * @brief 统计满足筛选条件的操作总条数（不受分页影响）。
     * @param query 查询筛选条件。
     * @return 满足条件的总条数。
     */
    [[nodiscard]] Result<int64_t> CountOperations(const schedule::QueryOperationCommand& query) const override;

   private:
    /** @brief 在调用方持有仓储锁时读取指定日程。 @param id 日程标识。 @return 日程或错误。 */
    Result<schedule::Schedule> FindByIdLocked(schedule::ScheduleId id) const;

    SqliteDatabase& database_;
    mutable std::mutex mutex_;
};

}  // namespace voicelife::storage_sqlite
