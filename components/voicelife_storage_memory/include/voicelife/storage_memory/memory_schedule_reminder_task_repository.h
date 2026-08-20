#pragma once

#include <mutex>

#include "voicelife/schedule/schedule_reminder_task_repository.h"

namespace voicelife::storage_memory {

/// Volatile reminder-task repository used by host and memory profiles.
class MemoryScheduleReminderTaskRepository final : public schedule::ScheduleReminderTaskRepository {
   public:
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
