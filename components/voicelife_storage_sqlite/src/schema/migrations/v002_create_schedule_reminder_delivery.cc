#include "schema/migrations/v002_create_schedule_reminder_delivery.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

constexpr char kCreateScheduleReminderDelivery[] = R"sql(
CREATE TABLE schedule_reminder_delivery (
    schedule_id INTEGER PRIMARY KEY REFERENCES schedule(id) ON DELETE CASCADE,
    delivered_at INTEGER NOT NULL
);
)sql";

}  // namespace

Status ApplyV002CreateScheduleReminderDelivery(SqliteDatabase& database) {
    return database.Execute(kCreateScheduleReminderDelivery);
}

}  // namespace voicelife::storage_sqlite::schema::migrations
