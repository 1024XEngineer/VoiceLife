#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/// 周期规则及其关联例外与未来发生时间的查询视图。
struct ScheduleRuleView {
    ScheduleRule rule;
    std::vector<ScheduleException> exceptions;
    std::vector<DateTime> upcoming_occurrences;
};

/// 创建周期规则的返回数据。
struct CreateScheduleRuleResult {
    Status status;
    std::optional<ScheduleRule> rule;
    std::vector<Schedule> schedules;
    std::vector<Schedule> conflicts;
    std::string error;
};

/// 查询周期规则的返回数据。
struct QueryScheduleRulesResult {
    Status status;
    std::vector<ScheduleRuleView> rules;
    int64_t total = 0;
    std::string error;
};

/// 修改整条周期规则的返回数据。
struct UpdateScheduleRuleResult {
    Status status;
    std::optional<ScheduleRule> rule;
    std::vector<Schedule> schedules;
    std::vector<Schedule> conflicts;
    std::string error;
};

/// 取消整条周期规则的返回数据。
struct CancelScheduleRuleResult {
    Status status;
    std::optional<ScheduleRule> rule;
    int64_t cancelled_count = 0;
    std::string error;
};

/// 修改周期中某一次的返回数据。
struct UpdateScheduleOccurrenceResult {
    Status status;
    std::optional<Schedule> schedule;
    std::optional<ScheduleException> exception;
    std::vector<Schedule> conflicts;
    std::string error;
};

/// 跳过周期中某一次的返回数据。
struct SkipScheduleOccurrenceResult {
    Status status;
    std::optional<Schedule> schedule;
    std::optional<ScheduleException> exception;
    std::string error;
};

/// 生成规则下一条实例的返回数据。
struct GenerateNextScheduleInstanceResult {
    Status status;
    std::optional<Schedule> schedule;
    std::string error;
};

}  // namespace voicelife::schedule
