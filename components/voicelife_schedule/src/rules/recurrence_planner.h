#pragma once

#include <optional>
#include <vector>

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 将 UTC 秒时间转换为东八区本地日期。
 * @param time UTC 时间。
 * @return 对应的本地日期。
 */
LocalDate LocalDateFromUtc(DateTime time);

/**
 * @brief 计算周期规则在 from（含）之后的第一个 occurrence。
 * @param rule 周期规则；调用前应保证规则参数已通过校验。
 * @param from 搜索起点（UTC 秒，包含）。
 * @return occurrence 时间（UTC 秒）；规则已结束（超过 end_date）或无匹配时返回空。
 */
std::optional<DateTime> NextOccurrence(const ScheduleRule& rule, DateTime from);

/**
 * @brief 展开周期规则在 [range_start, range_end) 内的 occurrence。
 * @param rule 周期规则；调用前应保证规则参数已通过校验。
 * @param range_start 左闭边界（UTC 秒）。
 * @param range_end 右开边界（UTC 秒）。
 * @param limit 最多返回的 occurrence 数量；默认 3，显式传入时最大会被收敛到 10。
 * @return 按时间升序排列的 occurrence（UTC 秒）。
 */
std::vector<DateTime> PlanOccurrences(const ScheduleRule& rule, DateTime range_start, DateTime range_end,
                                      int limit = 3);

}  // namespace voicelife::schedule
