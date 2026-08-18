#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite::mapping {

/**
 * @brief 将周期规则字段绑定到预编译语句（不含主键 id）。
 * @param statement 目标 SQLite 语句包装器。
 * @param rule 待绑定的规则。
 * @return 全部字段绑定成功时返回成功状态。
 */
Status BindScheduleRule(SqliteStatement& statement, const schedule::ScheduleRule& rule);

/**
 * @brief 从当前结果行读取周期规则实体。
 * @param statement 已执行并停在一行结果上的 SQLite 语句包装器。
 * @return 映射后的规则或数据格式错误。
 */
Result<schedule::ScheduleRule> ReadScheduleRule(const SqliteStatement& statement);

}  // namespace voicelife::storage_sqlite::mapping
