#pragma once

#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_rule_commands.h"
#include "voicelife/schedule/schedule_rule_repository.h"
#include "voicelife/schedule/schedule_rule_results.h"

namespace voicelife::schedule {

/**
 * @brief 提供周期规则的创建、查询、修改、取消及单次修改/跳过业务。
 *
 * 一次性日程仍由 ScheduleService 处理；本服务只处理周期规则与周期中的单次操作。
 */
class ScheduleRuleService {
   public:
    /**
     * @brief 使用指定仓储构造周期规则服务。
     * @param rule_repository 周期规则仓储；生命周期必须长于本服务。
     * @param exception_repository 单次例外仓储；生命周期必须长于本服务。
     * @param schedule_repository 日程实例仓储，用于物化实例和冲突检测。
     */
    ScheduleRuleService(ScheduleRuleRepository& rule_repository, ScheduleExceptionRepository& exception_repository,
                        ScheduleRepository& schedule_repository);

    /** @brief 创建周期规则并物化首条实例。 */
    CreateScheduleRuleResult create_schedule_rule(const CreateScheduleRuleCommand& command) const;

    /** @brief 查询周期规则及其例外与未来发生时间。 */
    QueryScheduleRulesResult query_schedule_rules(const QueryScheduleRulesCommand& command) const;

    /** @brief 修改整条周期规则并重建未来实例。 */
    UpdateScheduleRuleResult update_schedule_rule(const UpdateScheduleRuleCommand& command);

    /** @brief 取消整条周期规则及其未来实例。 */
    CancelScheduleRuleResult cancel_schedule_rule(const CancelScheduleRuleCommand& command);

    /** @brief 修改周期中的某一次。 */
    UpdateScheduleOccurrenceResult update_schedule_occurrence(const UpdateScheduleOccurrenceCommand& command);

    /** @brief 跳过周期中的某一次。 */
    SkipScheduleOccurrenceResult skip_schedule_occurrence(const SkipScheduleOccurrenceCommand& command);

    /** @brief 生成规则的下一条实例。 */
    GenerateNextScheduleInstanceResult generate_next_schedule_instance(
        const GenerateNextScheduleInstanceCommand& command);

   private:
    ScheduleRuleRepository& rule_repository_;
    ScheduleExceptionRepository& exception_repository_;
    ScheduleRepository& schedule_repository_;
};

}  // namespace voicelife::schedule
