#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::schema::migrations {

/**
 * @brief 执行版本二迁移，创建日程操作记录及其规范化快照列。
 * @param database 已打开且已进入迁移事务的数据库连接。
 * @return 迁移成功时返回成功状态，否则返回数据库错误。
 */
Status ApplyV002CreateScheduleOperation(SqliteDatabase& database);

}  // namespace voicelife::storage_sqlite::schema::migrations
