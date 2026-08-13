#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite {

/**
 * @brief 使用 SQLite 持久化日程实体的具体仓储。
 *
 * SQL 文本和 SQLite 行映射位于实现目录，业务服务只能看到 ScheduleRepository 接口。
 */
class SqliteScheduleRepository final : public schedule::ScheduleRepository {
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

    [[nodiscard]] Result<std::optional<schedule::Schedule>> FindByIdempotencyKey(std::string_view key) const override;
    Result<schedule::Schedule> InsertOnce(const schedule::Schedule& schedule, std::string_view key) override;

    /** @brief 更新一条日程。 @param schedule 待更新日程。 @return 更新状态。 */
    Status Update(const schedule::Schedule& schedule) override;

    /** @brief 删除一条日程。 @param id 日程标识。 @return 删除状态。 */
    Status Delete(schedule::ScheduleId id) override;

    /**
     * @brief 读取全部日程。
     * @return 按开始时间和标识排序的日程集合。
     */
    [[nodiscard]] Result<std::vector<schedule::Schedule>> FindAll() const override;

    /** @brief 原子领取到期且未投递的有效日程提醒。 */
    Result<std::vector<schedule::DueScheduleReminder>> ClaimDueReminders(schedule::DateTime now,
                                                                         std::size_t limit) override;

   private:
    SqliteDatabase& database_;
};

}  // namespace voicelife::storage_sqlite
