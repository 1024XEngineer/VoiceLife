#pragma once

namespace voicelife::storage_sqlite::sql {

/** @brief 按 (rule_id, original_start_time) 插入或更新一条单次例外。 */
extern const char kUpsertScheduleException[];
/** @brief 读取某规则的全部例外。 */
extern const char kFindExceptionsByRule[];
/** @brief 按逻辑键读取一条例外。 */
extern const char kFindExceptionByRuleAndTime[];
/** @brief 删除某规则在指定时间之后的未发生例外。 */
extern const char kDeleteFutureExceptionsByRule[];

}  // namespace voicelife::storage_sqlite::sql
