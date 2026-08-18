#pragma once

#include <optional>

#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 查找某条周期规则在指定原始发生时间是否已经物化为日程实例。
 * @param repository 日程仓储。
 * @param rule_id 周期规则标识。
 * @param original_start_time 原始发生时间。
 * @param exception_schedule_id 单次例外中已经关联的日程标识；有值时优先按 ID 读取。
 * @return 已物化实例；不存在或关联 ID 已失效时 value 为空。
 */
Result<std::optional<Schedule>> FindMaterializedScheduleOccurrence(ScheduleRepository& repository,
                                                                   ScheduleRuleId rule_id, DateTime original_start_time,
                                                                   std::optional<ScheduleId> exception_schedule_id);

}  // namespace voicelife::schedule
