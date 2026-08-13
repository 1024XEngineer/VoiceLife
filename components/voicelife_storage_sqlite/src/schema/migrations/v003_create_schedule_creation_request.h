#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::schema::migrations {

/** @brief 创建外部日程创建请求与首次写入日程的持久化映射。 */
Status ApplyV003CreateScheduleCreationRequest(SqliteDatabase& database);

}  // namespace voicelife::storage_sqlite::schema::migrations
