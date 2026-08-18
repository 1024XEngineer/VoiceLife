#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::mapping {

/**
 * @brief 将操作记录字段绑定到操作 INSERT 语句。
 * @param statement 目标 SQLite 预编译语句。
 * @param operation 待绑定的操作记录。
 * @return 全部字段绑定成功时返回成功状态。
 */
Status BindOperation(SqliteStatement& statement, const schedule::OperationRecord& operation);

/**
 * @brief 从操作查询结果行读取操作记录。
 * @param statement 已执行并停在操作结果行上的 SQLite 语句。
 * @return 映射后的操作记录或数据格式错误。
 */
Result<schedule::OperationRecord> ReadOperation(const SqliteStatement& statement);

}  // namespace voicelife::storage_sqlite::mapping
