#pragma once

namespace voicelife::storage_sqlite::sql {

/** @brief 插入一条周期规则。 */
extern const char kInsertScheduleRule[];
/** @brief 更新周期规则的全部持久化字段。 */
extern const char kUpdateScheduleRule[];
/** @brief 读取全部周期规则。 */
extern const char kFindAllScheduleRules[];
/** @brief 按主键读取一条周期规则。 */
extern const char kFindScheduleRuleById[];
/** @brief 将周期规则标记为取消。 */
extern const char kCancelScheduleRuleById[];
/** @brief 将某规则全部已创建实例标记为取消。 */
extern const char kCancelSchedulesByRule[];
/** @brief 物理删除某规则未发生的未来实例（用于整条规则重建）。 */
extern const char kDeleteFutureSchedulesByRule[];

}  // namespace voicelife::storage_sqlite::sql
