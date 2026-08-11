#pragma once

namespace voicelife::storage_sqlite::sql {

/** @brief 创建日程表及查询索引。 */
extern const char kCreateScheduleSchema[];

/** @brief 插入一条由数据库生成主键的日程。 */
extern const char kInsertSchedule[];

/** @brief 按开始时间和主键读取全部日程。 */
extern const char kFindAllSchedules[];

}  // namespace voicelife::storage_sqlite::sql
