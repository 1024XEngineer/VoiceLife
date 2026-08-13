#pragma once

#include "voicelife/schedule/schedule_rule_commands.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 将整条周期规则的三态字段补丁应用到已有规则。
 * @param command 修改周期规则命令。
 * @param rule 待修改的规则。
 */
void ApplyScheduleRulePatch(const UpdateScheduleRuleCommand& command, ScheduleRule& rule);

/**
 * @brief 将周期发生时间的三态字段补丁应用到单次例外。
 * @param command 修改周期发生时间命令。
 * @param exception 待修改的单次例外。
 */
void ApplyScheduleOccurrencePatch(const UpdateScheduleOccurrenceCommand& command, ScheduleException& exception);

}  // namespace voicelife::schedule
