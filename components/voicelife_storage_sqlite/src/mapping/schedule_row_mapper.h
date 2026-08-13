#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::mapping {

/**
 * @brief 将日程字段绑定到预编译语句。
 * @param statement 目标 SQLite 语句包装器。
 * @param schedule 待绑定的日程。
 * @return 全部字段绑定成功时返回成功状态。
 */
Status BindSchedule(SqliteStatement& statement, const schedule::Schedule& schedule);

/**
 * @brief 从当前结果行读取日程实体。
 * @param statement 已执行并停在一行结果上的 SQLite 语句包装器。
 * @return 映射后的日程或数据格式错误。
 */
Result<schedule::Schedule> ReadSchedule(const SqliteStatement& statement);

}  // namespace voicelife::storage_sqlite::mapping
