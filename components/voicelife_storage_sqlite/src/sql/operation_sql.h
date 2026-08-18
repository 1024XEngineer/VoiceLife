#pragma once

namespace voicelife::storage_sqlite::sql {

/** @brief 插入一条 active 操作记录，主键由 SQLite 生成。 */
extern const char kInsertOperation[];
/** @brief 查询十五分钟闭区间内仍 active 的操作记录。 */
extern const char kFindRecentOperations[];
/** @brief 按主键查询操作记录及 active 标记。 */
extern const char kFindOperationById[];
/** @brief 原子失效一条 active 操作记录。 */
extern const char kDeactivateOperation[];

}  // namespace voicelife::storage_sqlite::sql
