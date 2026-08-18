#pragma once

#include <string>

#include "voicelife/schedule/schedule_commands.h"

namespace voicelife::storage_sqlite::sql {

/** @brief 插入一条操作记录，主键由 SQLite 生成。 */
extern const char kInsertOperation[];

/** @brief 生成带筛选条件的操作查询 SQL（含分页与倒序排序）。 */
std::string BuildOperationFindSql(const schedule::QueryOperationCommand& query);

/** @brief 生成带筛选条件的操作总数 SQL（不受分页影响）。 */
std::string BuildOperationCountSql(const schedule::QueryOperationCommand& query);

}  // namespace voicelife::storage_sqlite::sql
