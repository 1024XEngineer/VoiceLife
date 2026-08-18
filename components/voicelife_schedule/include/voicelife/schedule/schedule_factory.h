#pragma once

#include <cstdint>

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_rule_commands.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 将本地时刻转换为当日 0 点起的秒数。
 * @param value 本地时刻。
 * @return 当日 0 点起的秒数。
 */
std::int64_t LocalTimeToSeconds(const LocalTime& value);

/** @brief 负责从命令或周期规则构造日程实例，并应用单次例外覆盖字段。 */
class ScheduleFactory {
   public:
    /**
     * @brief 从一次性日程命令构造领域实例。
     * @param command 创建日程命令。
     * @return 未生成 id 和时间戳的日程实体。
     */
    static Schedule CreateFromCommand(const CreateScheduleCommand& command);

    /**
     * @brief 从创建周期规则命令构造领域规则。
     * @param command 创建周期规则命令。
     * @return 未生成 id 和时间戳的周期规则实体。
     */
    static ScheduleRule CreateRuleFromCommand(const CreateScheduleRuleCommand& command);

    /**
     * @brief 从周期规则构造某次发生对应的日程实例。
     * @param rule 周期规则。
     * @param occurrence 本次发生时间。
     * @return 未生成 id 和时间戳的日程实例。
     */
    static Schedule CreateOccurrence(const ScheduleRule& rule, DateTime occurrence);

    /**
     * @brief 将单次例外覆盖字段应用到日程实例。
     * @param schedule 要修改的日程实例。
     * @param exception 单次例外。
     */
    static void ApplyOverride(Schedule& schedule, const ScheduleException& exception);
};

}  // namespace voicelife::schedule
