#pragma once

#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 返回创建日程时用于冲突判断的模拟数据。
 * @return 固定的有效日程；数据库接入后删除该模拟入口。
 */
std::vector<Schedule> LoadMockSchedulesForCreate();

/**
 * @brief 在模拟存储中将指定日程标记为已取消。
 * @param schedule_id 要取消的日程 ID。
 * @return 成功时返回保留全部原字段的已取消日程；不存在或已取消时返回失败。
 */
Result<Schedule> CancelMockSchedule(ScheduleId schedule_id);

}  // namespace voicelife::schedule
