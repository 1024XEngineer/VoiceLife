#pragma once

#include <cstdint>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_rule_results.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

CreateScheduleRuleResult FailedCreateScheduleRuleResult(Status status,
                                                        std::vector<Schedule> conflicts = {});
QueryScheduleRulesResult FailedQueryScheduleRulesResult(Status status);
UpdateScheduleRuleResult FailedUpdateScheduleRuleResult(Status status,
                                                        std::vector<Schedule> conflicts = {});
CancelScheduleRuleResult FailedCancelScheduleRuleResult(Status status, int64_t cancelled_count = 0);
UpdateScheduleOccurrenceResult FailedUpdateScheduleOccurrenceResult(Status status);
SkipScheduleOccurrenceResult FailedSkipScheduleOccurrenceResult(Status status);
GenerateNextScheduleInstanceResult FailedGenerateNextScheduleInstanceResult(Status status);

}  // namespace voicelife::schedule
