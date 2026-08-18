#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::schema::migrations {

/**
 * @brief 执行版本三迁移，创建周期规则表和单次例外表及索引。
 * @param database 已打开且已进入迁移事务的 SQLite 数据库连接。
 * @return 迁移成功时返回成功状态。
 */
Status ApplyV003CreateScheduleRule(SqliteDatabase& database);

}  // namespace voicelife::storage_sqlite::schema::migrations
