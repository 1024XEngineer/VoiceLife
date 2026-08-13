#include "schedule_rule_result_helpers.h"

#include <string>
#include <utility>

namespace voicelife::schedule {
namespace {

std::string ErrorFrom(const Status& status) { return status.message; }

}  // namespace

CreateScheduleRuleResult FailedCreateScheduleRuleResult(Status status, std::vector<Schedule> conflicts) {
    const std::string error = ErrorFrom(status);
    return {
        .status = std::move(status),
        .rule = std::nullopt,
        .schedules = {},
        .conflicts = std::move(conflicts),
        .error = error,
    };
}

QueryScheduleRulesResult FailedQueryScheduleRulesResult(Status status) {
    const std::string error = ErrorFrom(status);
    return {
        .status = std::move(status),
        .rules = {},
        .total = 0,
        .error = error,
    };
}

UpdateScheduleRuleResult FailedUpdateScheduleRuleResult(Status status, std::vector<Schedule> conflicts) {
    const std::string error = ErrorFrom(status);
    return {
        .status = std::move(status),
        .rule = std::nullopt,
        .schedules = {},
        .conflicts = std::move(conflicts),
        .error = error,
    };
}

CancelScheduleRuleResult FailedCancelScheduleRuleResult(Status status, int64_t cancelled_count) {
    const std::string error = ErrorFrom(status);
    return {
        .status = std::move(status),
        .rule = std::nullopt,
        .cancelled_count = cancelled_count,
        .error = error,
    };
}

UpdateScheduleOccurrenceResult FailedUpdateScheduleOccurrenceResult(Status status) {
    const std::string error = ErrorFrom(status);
    return {
        .status = std::move(status),
        .schedule = std::nullopt,
        .exception = std::nullopt,
        .conflicts = {},
        .error = error,
    };
}

SkipScheduleOccurrenceResult FailedSkipScheduleOccurrenceResult(Status status) {
    const std::string error = ErrorFrom(status);
    return {
        .status = std::move(status),
        .schedule = std::nullopt,
        .exception = std::nullopt,
        .error = error,
    };
}

GenerateNextScheduleInstanceResult FailedGenerateNextScheduleInstanceResult(Status status) {
    const std::string error = ErrorFrom(status);
    return {
        .status = std::move(status),
        .schedule = std::nullopt,
        .error = error,
    };
}

}  // namespace voicelife::schedule
