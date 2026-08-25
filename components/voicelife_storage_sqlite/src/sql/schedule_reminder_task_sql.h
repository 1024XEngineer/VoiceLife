#pragma once

namespace voicelife::storage_sqlite::sql {
extern const char kInsertScheduleReminderTask[];
extern const char kFindAllScheduleReminderTasks[];
extern const char kFindScheduleReminderTaskById[];
extern const char kFindScheduleReminderTaskByTimingTaskId[];
extern const char kFindScheduleReminderTasksBySchedule[];
extern const char kFindTriggeredScheduleReminderTasks[];
extern const char kUpdateScheduleReminderTask[];
}  // namespace voicelife::storage_sqlite::sql
