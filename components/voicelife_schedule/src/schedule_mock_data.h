#pragma once

#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 返回日程模块用于查询和冲突判断的模拟数据。
 * @return 固定的有效日程；数据库接入后删除该模拟入口。
 */
std::vector<Schedule> LoadMockSchedules();

/**
 * @brief 返回创建日程时用于冲突判断的共享模拟数据。
 * @return 当前模拟存储中的日程副本；数据库接入后删除该模拟入口。
 */
std::vector<Schedule> LoadMockSchedulesForCreate();

/**
 * @brief 返回查询日程时使用的模拟数据。
 * @return 包含有效、已完成和已取消日程的固定数据；数据库接入后删除该模拟入口。
 */
std::vector<Schedule> LoadMockSchedulesForQuery();

/**
 * @brief 按日程 ID 读取模拟存储中的完整日程。
 * @param schedule_id 要读取的日程 ID。
 * @return 找到时返回日程副本；日程不存在时返回失败。
 */
Result<Schedule> FindMockScheduleById(ScheduleId schedule_id);

/**
 * @brief 从模拟存储中物理移除指定日程。
 * @param schedule_id 要移除的日程 ID。
 * @return 成功时返回移除前的完整日程；日程不存在时返回失败。
 */
Result<Schedule> RemoveMockSchedule(ScheduleId schedule_id);

/**
 * @brief 使用完整快照恢复模拟日程，存在同 ID 日程时覆盖，否则插入。
 * @param snapshot 要恢复的完整日程快照。
 * @return 成功时返回恢复后的日程；快照 ID 非法时返回失败。
 */
Result<Schedule> RestoreMockSchedule(const Schedule& snapshot);

/**
 * @brief 在模拟存储中将指定日程标记为已取消。
 * @param schedule_id 要取消的日程 ID。
 * @return 成功时返回保留全部原字段的已取消日程；不存在或已取消时返回失败。
 */
Result<Schedule> CancelMockSchedule(ScheduleId schedule_id);

/**
 * @brief 将模拟日程存储重置为模块默认数据。
 * @return 无。
 */
void ResetMockSchedulesForTesting();

/**
 * @brief 使用指定数据替换模拟日程存储，供测试构造隔离场景。
 * @param schedules 要写入模拟存储的完整日程集合。
 * @return 无。
 */
void SeedMockSchedulesForTesting(std::vector<Schedule> schedules);

}  // namespace voicelife::schedule
