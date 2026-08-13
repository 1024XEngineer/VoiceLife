#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::schema::migrations {

/** @brief 创建一次性日程提醒投递事实表。 */
Status ApplyV002CreateScheduleReminderDelivery(SqliteDatabase& database);

}  // namespace voicelife::storage_sqlite::schema::migrations
