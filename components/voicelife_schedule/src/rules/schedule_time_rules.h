#pragma once

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 判断两个有时间的日程是否冲突。
 * @param left 第一个日程。
 * @param right 第二个日程。
 * @return 时间区间相交或时间点重合时返回 true；首尾相接返回 false。
 */
bool SchedulesConflict(const Schedule& left, const Schedule& right);

/**
 * @brief 判断两个不冲突日程是否临近。
 * @param left 第一个日程。
 * @param right 第二个日程。
 * @return 两个时间范围最短间隔不超过十五分钟时返回 true。
 */
bool SchedulesAreNearby(const Schedule& left, const Schedule& right);

}  // namespace voicelife::schedule
