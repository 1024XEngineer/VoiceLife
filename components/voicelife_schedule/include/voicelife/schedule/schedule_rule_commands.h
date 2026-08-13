#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/// 可清空字段的三态修改值：外层无值表示不修改，内层无值表示清空。
template <typename T>
using FieldPatch = std::optional<std::optional<T>>;

/// 创建周期规则所需的数据。
struct CreateScheduleRuleCommand {
    std::string event;
    Frequency freq_type = Frequency::kDaily;
    LocalTime start_time;
    std::optional<LocalDate> start_date;
    std::optional<LocalTime> end_time;
    std::optional<std::string> location;
    std::optional<std::string> notes;
    int32_t interval_val = 1;
    std::optional<uint8_t> weekdays_mask;
    std::optional<uint8_t> day_of_month;
    std::optional<uint8_t> month_of_year;
    std::optional<MonthlyMode> monthly_mode;
    std::optional<LocalDate> end_date;
    std::optional<int32_t> occurrence_count;
    bool ignore_conflict = false;
};

/// 查询周期规则所需的筛选和分页条件。
struct QueryScheduleRulesCommand {
    std::optional<ScheduleRuleId> rule_id;
    std::optional<std::string> keyword;
    ScheduleStatusFilter status = ScheduleStatusFilter::kActive;
    int64_t limit = 10;
    int64_t offset = 0;
};

/// 修改整条周期规则所需的数据；未提供字段保持原值。
struct UpdateScheduleRuleCommand {
    ScheduleRuleId rule_id = 0;
    std::optional<std::string> event;
    FieldPatch<std::string> location;
    FieldPatch<std::string> notes;
    std::optional<Frequency> freq_type;
    std::optional<int32_t> interval_val;
    FieldPatch<uint8_t> weekdays_mask;
    FieldPatch<uint8_t> day_of_month;
    FieldPatch<uint8_t> month_of_year;
    FieldPatch<MonthlyMode> monthly_mode;
    std::optional<LocalTime> start_time;
    FieldPatch<LocalDate> start_date;
    FieldPatch<LocalTime> end_time;
    FieldPatch<LocalDate> end_date;
    FieldPatch<int32_t> occurrence_count;
    bool ignore_conflict = false;
};

/// 取消整条周期规则所需的数据。
struct CancelScheduleRuleCommand {
    ScheduleRuleId rule_id = 0;
};

/// 修改周期中的某一次所需的数据。
struct UpdateScheduleOccurrenceCommand {
    ScheduleRuleId rule_id = 0;
    DateTime original_start_time;
    FieldPatch<std::string> event;
    FieldPatch<DateTime> start_time;
    FieldPatch<DateTime> end_time;
    FieldPatch<std::string> location;
    FieldPatch<std::string> notes;
    bool ignore_conflict = false;
};

/// 跳过周期中的某一次所需的数据。
struct SkipScheduleOccurrenceCommand {
    ScheduleRuleId rule_id = 0;
    DateTime original_start_time;
};

/// 生成规则下一条实例所需的数据。
struct GenerateNextScheduleInstanceCommand {
    ScheduleRuleId rule_id = 0;
};

}  // namespace voicelife::schedule
