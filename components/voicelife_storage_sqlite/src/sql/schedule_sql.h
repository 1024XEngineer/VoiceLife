#pragma once

#include <string>

#include "voicelife/schedule/schedule_commands.h"

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

/** @brief 生成带筛选条件的日程查询 SQL。 */
std::string BuildScheduleFindSql(const schedule::QueryScheduleCommand& query);

/** @brief 生成带筛选条件的日程总数 SQL。 */
std::string BuildScheduleCountSql(const schedule::QueryScheduleCommand& query);

/** @brief 查询可能重叠的时间窗口日程。 */
extern const char kFindOverlappingSchedules[];

}  // namespace voicelife::storage_sqlite::sql
