#pragma once

#include <vector>

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 返回创建日程时用于冲突判断的模拟数据。
 * @return 固定的有效日程；数据库接入后删除该模拟入口。
 */
std::vector<Schedule> LoadMockSchedulesForCreate();

/**
 * @brief 返回查询日程时使用的模拟数据。
 * @return 包含有效、已完成和已取消日程的固定数据；数据库接入后删除该模拟入口。
 */
std::vector<Schedule> LoadMockSchedulesForQuery();

}  // namespace voicelife::schedule
