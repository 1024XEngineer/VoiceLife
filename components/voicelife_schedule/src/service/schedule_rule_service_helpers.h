#pragma once

#include <optional>
#include <string>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule::schedule_rule_service_helpers {

/** @brief 计算规则在指定时间之后的前 n 次发生时间。 @param rule 周期规则。 @param from 搜索起点。 @param n 数量。
 * @return 发生时间列表。 */
std::vector<DateTime> NextOccurrences(const ScheduleRule& rule, DateTime from, int n);

/** @brief 校验与开始日期无关的规则字段。 @param rule 周期规则。 @return 校验状态。 */
Status ValidateRuleFields(const ScheduleRule& rule);

/** @brief 校验依赖开始日期的规则字段。 @param rule 周期规则。 @return 校验状态。 */
Status ValidateRuleDateRange(const ScheduleRule& rule);

/** @brief 判断关键词是否命中规则。 @param rule 周期规则。 @param keyword 关键词。 @return 命中时返回 true。 */
bool MatchesKeyword(const ScheduleRule& rule, const std::string& keyword);

/** @brief 判断规则状态是否命中筛选。 @param rule 周期规则。 @param filter 状态筛选。 @return 命中时返回 true。 */
bool MatchesStatus(const ScheduleRule& rule, ScheduleStatusFilter filter);

/** @brief 根据本地日期和时间构造东八区时间点。 @param date 本地日期。 @param time 本地时间。 @return 日程时间。 */
DateTime AtLocalDate(const LocalDate& date, const LocalTime& time);

/** @brief 获取当前秒级系统时间。 @return 当前时间。 */
DateTime Now();

}  // namespace voicelife::schedule::schedule_rule_service_helpers
