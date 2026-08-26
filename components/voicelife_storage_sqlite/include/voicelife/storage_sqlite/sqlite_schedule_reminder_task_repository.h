#pragma once

#include "voicelife/schedule/schedule_reminder_task_repository.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite {

/** @brief 使用 SQLite 持久化独立的日程提醒任务记录。 */
class SqliteScheduleReminderTaskRepository final : public schedule::ScheduleReminderTaskRepository {
   public:
    /** @brief 构造 SQLite 提醒任务仓储。
     * @param database SQLite 数据库实例。
     */
    explicit SqliteScheduleReminderTaskRepository(SqliteDatabase& database);

    /** @brief 插入提醒任务。
     * @param task 待插入任务。
     * @return 插入后的任务或错误状态。
     */
    Result<schedule::ScheduleReminderTask> Insert(const schedule::ScheduleReminderTask& task) override;
    /** @brief 更新提醒任务。
     * @param task 待更新任务。
     * @return 更新操作状态。
     */
    Status Update(const schedule::ScheduleReminderTask& task) override;
    /** @brief 按标识查询任务。
     * @param id 任务标识。
     * @return 查询结果。
     */
    [[nodiscard]] Result<schedule::ScheduleReminderTask> FindById(int64_t id) const override;
    /** @brief 按 Timing task 标识查询任务。
     * @param timing_task_id Timing 层不透明任务标识。
     * @return 查询结果。
     */
    [[nodiscard]] Result<schedule::ScheduleReminderTask> FindByTimingTaskId(
        std::string_view timing_task_id) const override;
    /** @brief 查询日程的提醒任务。
     * @param schedule_id 日程标识。
     * @return 查询结果。
     */
    [[nodiscard]] Result<std::vector<schedule::ScheduleReminderTask>> FindBySchedule(
        schedule::ScheduleId schedule_id) const override;
    /** @brief 查询全部提醒任务。
     * @return 查询结果。
     */
    [[nodiscard]] Result<std::vector<schedule::ScheduleReminderTask>> FindAll() const override;
    /** @brief 查询时间范围内已触发任务。
     * @param from 起始时间。
     * @param to 结束时间。
     * @return 查询结果。
     */
    [[nodiscard]] Result<std::vector<schedule::ScheduleReminderTask>> FindTriggered(
        schedule::DateTime from, schedule::DateTime to) const override;

   private:
    SqliteDatabase& database_;
    mutable std::mutex mutex_;
};

}  // namespace voicelife::storage_sqlite
