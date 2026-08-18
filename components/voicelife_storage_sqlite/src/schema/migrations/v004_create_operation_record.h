#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::schema::migrations {

/**
 * @brief 执行版本四迁移，重建操作记录表为纯审计日志结构。
 *
 * 旧的 v002 表使用摊平的前置快照列（previous_*）与 active 软删除；
 * 新表只保存 opaque 的 before JSON 快照，删除 active 与撤销相关列。
 * @param database 已打开且已进入迁移事务的 SQLite 数据库连接。
 * @return 迁移成功时返回成功状态。
 */
Status ApplyV004CreateOperationRecord(SqliteDatabase& database);

}  // namespace voicelife::storage_sqlite::schema::migrations
