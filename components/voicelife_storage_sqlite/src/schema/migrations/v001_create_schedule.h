#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::schema::migrations {

/**
 * @brief 执行版本一迁移，创建日程实例表。
 * @param database 已打开且已进入迁移事务的 SQLite 数据库连接。
 * @return 日程实例表创建成功时返回成功状态。
 */
Status ApplyV001CreateSchedule(SqliteDatabase& database);

}  // namespace voicelife::storage_sqlite::schema::migrations
