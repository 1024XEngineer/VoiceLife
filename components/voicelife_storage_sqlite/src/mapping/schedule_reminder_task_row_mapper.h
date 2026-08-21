#pragma once

#include "voicelife/schedule/schedule_reminder_task_repository.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite {
namespace mapping {
Status BindScheduleReminderTask(SqliteStatement& statement, const schedule::ScheduleReminderTask& task);
Result<schedule::ScheduleReminderTask> ReadScheduleReminderTask(const SqliteStatement& statement);
}  // namespace mapping
}  // namespace voicelife::storage_sqlite
