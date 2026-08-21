#pragma once

#include "voicelife/schedule/schedule_reminder_task_repository.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite {

/// @brief 使用 SQLite 持久化独立的日程提醒任务记录。
class SqliteScheduleReminderTaskRepository final : public schedule::ScheduleReminderTaskRepository {
   public:
    /// @brief 构造 SQLite 提醒任务仓储。
    /// @param database SQLite 数据库实例。
    explicit SqliteScheduleReminderTaskRepository(SqliteDatabase& database);

    Result<schedule::ScheduleReminderTask> Insert(const schedule::ScheduleReminderTask& task) override;
    Status Update(const schedule::ScheduleReminderTask& task) override;
    [[nodiscard]] Result<schedule::ScheduleReminderTask> FindById(int64_t id) const override;
    [[nodiscard]] Result<std::vector<schedule::ScheduleReminderTask>> FindBySchedule(
        schedule::ScheduleId schedule_id) const override;
    [[nodiscard]] Result<std::vector<schedule::ScheduleReminderTask>> FindAll() const override;
    [[nodiscard]] Result<std::vector<schedule::ScheduleReminderTask>> FindTriggered(
        schedule::DateTime from, schedule::DateTime to) const override;

   private:
    SqliteDatabase& database_;
    mutable std::mutex mutex_;
};

}  // namespace voicelife::storage_sqlite
   /// @brief 插入提醒任务。
   /// @brief 更新提醒任务。
   /// @brief 按标识查询任务。
   /// @brief 查询日程的提醒任务。
   /// @brief 查询全部提醒任务。
   /// @brief 查询时间范围内已触发任务。
