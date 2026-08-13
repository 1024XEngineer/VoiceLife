#pragma once

namespace voicelife::storage_sqlite::sql {

/** @brief 插入一条由数据库生成主键的日程。 */
extern const char kInsertSchedule[];
extern const char kUpdateSchedule[];
extern const char kCancelSchedule[];
extern const char kFindScheduleById[];
extern const char kDeleteSchedulePhysical[];
extern const char kRestoreScheduleInsert[];
extern const char kRestoreScheduleUpdate[];

/** @brief 按开始时间和主键读取全部日程。 */
extern const char kFindAllSchedules[];

}  // namespace voicelife::storage_sqlite::sql
