#include "voicelife/schedule/schedule_factory.h"

#include <chrono>

namespace voicelife::schedule {

// 将本地时间换算成当天秒数，规则实例用它把结束时间偏移附加到发生时间上。
std::int64_t LocalTimeToSeconds(const LocalTime& value) {
    return static_cast<std::int64_t>(value.hour) * 3600 + static_cast<std::int64_t>(value.minute) * 60 +
           static_cast<std::int64_t>(value.second);
}

// 从创建命令组装一次性日程，保持实体字段集中在工厂内初始化。
Schedule ScheduleFactory::CreateFromCommand(const CreateScheduleCommand& command) {
    Schedule schedule;
    schedule.id = 0;
    schedule.event = command.event;
    schedule.start_time = command.start_time;
    schedule.end_time = command.end_time;
    schedule.location = command.location;
    schedule.notes = command.notes;
    schedule.rule_id = std::nullopt;
    schedule.status = ScheduleStatus::kActive;
    return schedule;
}

// 从创建规则命令组装周期规则，集中管理规则字段初始化和 active 状态。
ScheduleRule ScheduleFactory::CreateRuleFromCommand(const CreateScheduleRuleCommand& command) {
    ScheduleRule rule;
    rule.id = 0;
    rule.event = command.event;
    rule.location = command.location;
    rule.notes = command.notes;
    rule.freq_type = command.freq_type;
    rule.interval_val = command.interval_val;
    rule.weekdays_mask = command.weekdays_mask;
    rule.day_of_month = command.day_of_month;
    rule.month_of_year = command.month_of_year;
    rule.monthly_mode = command.monthly_mode;
    rule.start_time = command.start_time;
    rule.start_date = command.start_date.value_or(schedule::LocalDate{});
    rule.end_time = command.end_time;
    rule.end_date = command.end_date;
    rule.occurrence_count = command.occurrence_count;
    rule.status = ScheduleStatus::kActive;
    return rule;
}

// 根据周期规则和某次发生时间物化一条日程实例，同时计算规则定义的时间长度。
Schedule ScheduleFactory::CreateOccurrence(const ScheduleRule& rule, DateTime occurrence) {
    Schedule schedule;
    schedule.id = 0;
    schedule.event = rule.event;
    schedule.start_time = occurrence;
    if (rule.end_time.has_value()) {
        const std::int64_t duration = LocalTimeToSeconds(*rule.end_time) - LocalTimeToSeconds(rule.start_time);
        schedule.end_time = occurrence + std::chrono::seconds{duration};
    }
    schedule.location = rule.location;
    schedule.notes = rule.notes;
    schedule.rule_id = std::nullopt;
    schedule.status = ScheduleStatus::kActive;
    return schedule;
}

// 将单次例外里的覆盖字段写回已物化实例，供修改/生成单次日程时使用。
void ScheduleFactory::ApplyOverride(Schedule& schedule, const ScheduleException& exception) {
    if (exception.override_start_time.has_value()) schedule.start_time = exception.override_start_time;
    if (exception.override_end_time.has_value()) schedule.end_time = exception.override_end_time;
    if (exception.override_event.has_value()) schedule.event = *exception.override_event;
    if (exception.override_location.has_value()) schedule.location = exception.override_location;
    if (exception.override_notes.has_value()) schedule.notes = exception.override_notes;
}

}  // namespace voicelife::schedule
