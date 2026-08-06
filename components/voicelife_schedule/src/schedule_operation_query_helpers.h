#pragma once

#include <vector>

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 筛选当前时间往前十五分钟内的日程操作并按时间倒序排列。
 * @param operations 待筛选的日程操作记录。
 * @param now 当前秒级时间，用于计算查询窗口。
 * @return 位于十五分钟窗口内的操作记录；同秒操作按记录 ID 倒序排列。
 */
std::vector<OperationRecord> FilterRecentScheduleOperations(std::vector<OperationRecord> operations, DateTime now);

}  // namespace voicelife::schedule
