#include "schema/migrations/v007_add_schedule_reminder_task_event.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {
constexpr char kAddScheduleReminderTaskEvent[] = R"sql(
ALTER TABLE schedule_reminder_task ADD COLUMN event TEXT NOT NULL DEFAULT '';
UPDATE schedule_reminder_task
SET event = COALESCE((SELECT event FROM schedule WHERE schedule.id = schedule_reminder_task.schedule_id), '');
)sql";
}

Status ApplyV007AddScheduleReminderTaskEvent(SqliteDatabase& database) {
    return database.Execute(kAddScheduleReminderTaskEvent);
}

}  // namespace voicelife::storage_sqlite::schema::migrations
