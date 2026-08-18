#include "schedule_rule_update_helpers.h"

namespace voicelife::schedule {
namespace {

template <typename T>
void ApplyPatch(const FieldPatch<T>& patch, std::optional<T>& target) {
    if (patch.has_value()) target = *patch;
}

template <typename T>
void ApplyReplace(const std::optional<T>& replacement, T& target) {
    if (replacement.has_value()) target = *replacement;
}

}  // namespace

// 规则更新集中维护一次字段覆盖，避免服务方法内出现大段 if-optional 赋值。
void ApplyScheduleRulePatch(const UpdateScheduleRuleCommand& command, ScheduleRule& rule) {
    ApplyReplace(command.event, rule.event);
    ApplyPatch(command.location, rule.location);
    ApplyPatch(command.notes, rule.notes);
    ApplyReplace(command.freq_type, rule.freq_type);
    ApplyReplace(command.interval_val, rule.interval_val);
    ApplyPatch(command.weekdays_mask, rule.weekdays_mask);
    ApplyPatch(command.day_of_month, rule.day_of_month);
    ApplyPatch(command.month_of_year, rule.month_of_year);
    ApplyPatch(command.monthly_mode, rule.monthly_mode);
    ApplyReplace(command.start_time, rule.start_time);
    if (command.start_date.has_value()) rule.start_date = *(*command.start_date);
    ApplyPatch(command.end_time, rule.end_time);
    ApplyPatch(command.end_date, rule.end_date);
    ApplyPatch(command.occurrence_count, rule.occurrence_count);
}

// 单次例外更新也集中覆盖，未来增加字段时只需改这里和服务组装逻辑。
void ApplyScheduleOccurrencePatch(const UpdateScheduleOccurrenceCommand& command, ScheduleException& exception) {
    ApplyPatch(command.event, exception.override_event);
    ApplyPatch(command.start_time, exception.override_start_time);
    ApplyPatch(command.end_time, exception.override_end_time);
    ApplyPatch(command.location, exception.override_location);
    ApplyPatch(command.notes, exception.override_notes);
}

}  // namespace voicelife::schedule
