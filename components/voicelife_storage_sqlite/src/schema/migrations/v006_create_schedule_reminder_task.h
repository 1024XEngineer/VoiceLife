#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::schema::migrations {
Status ApplyV006CreateScheduleReminderTask(SqliteDatabase& database);
}
