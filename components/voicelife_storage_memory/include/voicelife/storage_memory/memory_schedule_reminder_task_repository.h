#pragma once

#include <mutex>

#include "voicelife/schedule/schedule_reminder_task_repository.h"

namespace voicelife::storage_memory {

/// @brief 供主机和内存配置使用的易失提醒任务仓储。
class MemoryScheduleReminderTaskRepository final : public schedule::ScheduleReminderTaskRepository {
   public:
    /// @brief 插入提醒任务。
    Result<schedule::ScheduleReminderTask> Insert(const schedule::ScheduleReminderTask& task) override;
    Status Update(const schedule::ScheduleReminderTask& task) override;
    [[nodiscard]] Result<schedule::ScheduleReminderTask> FindById(int64_t id) const override;
    [[nodiscard]] Result<std::vector<schedule::ScheduleReminderTask>> FindBySchedule(
        schedule::ScheduleId schedule_id) const override;
    [[nodiscard]] Result<std::vector<schedule::ScheduleReminderTask>> FindAll() const override;
    [[nodiscard]] Result<std::vector<schedule::ScheduleReminderTask>> FindTriggered(
        schedule::DateTime from, schedule::DateTime to) const override;

   private:
    mutable std::mutex mutex_;
    std::vector<schedule::ScheduleReminderTask> tasks_;
    int64_t next_id_ = 1;
};

}  // namespace voicelife::storage_memory
   /// @brief 更新提醒任务。
   /// @brief 按标识查询任务。
   /// @brief 查询日程的提醒任务。
   /// @brief 查询全部提醒任务。
   /// @brief 查询时间范围内已触发任务。
