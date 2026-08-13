#pragma once

namespace voicelife::storage_sqlite::sql {

/** @brief 插入一条由数据库生成主键的日程。 */
extern const char kInsertSchedule[];
extern const char kUpdateSchedule[];
extern const char kDeleteSchedule[];

/** @brief 按开始时间和主键读取全部日程。 */
extern const char kFindAllSchedules[];

/** @brief 查询截至指定时间尚未投递的有效日程。 */
extern const char kFindDueSchedules[];

/** @brief 将一条日程标记为已经投递；重复调用不会覆盖原投递时间。 */
extern const char kInsertReminderDelivery[];

/** @brief 按稳定创建键读取首次创建的日程。 */
extern const char kFindScheduleByIdempotencyKey[];

/** @brief 写入创建键与日程的持久化映射；同键重复写入会被唯一约束拒绝。 */
extern const char kInsertScheduleIdempotencyKey[];

}  // namespace voicelife::storage_sqlite::sql
