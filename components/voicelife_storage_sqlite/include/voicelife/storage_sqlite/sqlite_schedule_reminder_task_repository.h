#pragma once

#include "voicelife/storage_sqlite/sqlite_database.h"
#include "voicelife/schedule/schedule_reminder_task_repository.h"

namespace voicelife::storage_sqlite {

/// SQLite persistence for independent schedule reminder task records.
class SqliteScheduleReminderTaskRepository final : public schedule::ScheduleReminderTaskRepository {
   public:
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
