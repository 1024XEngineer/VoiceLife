#pragma once

#include <chrono>
#include <optional>
#include <utility>
#include <vector>

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

/**
 * @brief 返回日程时间区间的结束时间；无结束时间的日程按单个时间点处理。
 * @param schedule 日程；调用前应保证包含开始时间。
 * @return 日程区间结束时间。
 */
DateTime ScheduleRangeEnd(const Schedule& schedule);

/**
 * @brief 返回候选日程可能重叠或临近的查询窗口。
 * @param schedule 候选日程；调用前应保证包含开始时间。
 * @return 以开始时间和结束时间为基准扩展十五分钟后的 [start, end] 窗口。
 */
std::pair<DateTime, DateTime> ScheduleNearbyWindow(const Schedule& schedule);

/**
 * @brief 从已有日程中筛选与候选日程冲突的有效日程。
 * @param candidate 候选日程，必须包含开始时间。
 * @param schedules 待筛选日程集合。
 * @param ignored_rule_id 冲突检测时忽略的规则实例；为空时不做规则级忽略。
 * @return 与候选日程时间重叠的有效日程。
 */
std::vector<Schedule> FindConflictingSchedules(const Schedule& candidate, const std::vector<Schedule>& schedules,
                                               std::optional<ScheduleRuleId> ignored_rule_id = std::nullopt);

/**
 * @brief 从已有日程中筛选与候选日程临近但不冲突的有效日程。
 * @param candidate 候选日程，必须包含开始时间。
 * @param schedules 待筛选日程集合。
 * @return 与候选日程开始时间相差不超过十五分钟的有效日程。
 */
std::vector<Schedule> FindNearbySchedules(const Schedule& candidate, const std::vector<Schedule>& schedules);

}  // namespace voicelife::schedule
