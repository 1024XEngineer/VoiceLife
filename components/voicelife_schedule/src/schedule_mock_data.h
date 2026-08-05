#pragma once

#include <vector>

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 返回日程模块用于查询和冲突判断的模拟数据。
 * @return 固定的有效日程；数据库接入后删除该模拟入口。
 */
std::vector<Schedule> LoadMockSchedules();

}  // namespace voicelife::schedule
