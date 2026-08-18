#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "voicelife/contracts/tool.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_factory.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::mcp::schedule_tool_output {

inline constexpr int64_t kTimezoneOffsetSeconds = 8 * 3600;

inline std::string FormatDateTime(schedule::DateTime value) {
    const int64_t local = value.time_since_epoch().count() + kTimezoneOffsetSeconds;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    schedule::CivilFromDays(local / 86400, year, month, day);
    const int64_t tod = local % 86400;
    hour = static_cast<int>(tod / 3600);
    minute = static_cast<int>((tod % 3600) / 60);
    second = static_cast<int>(tod % 60);

    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return buffer;
}

inline std::optional<schedule::DateTime> ParseDateTime(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return std::nullopt;
    }
    const int64_t days = schedule::DaysFromCivil(year, month, day);
    return schedule::DateTime{
        std::chrono::seconds{days * 86400 + hour * 3600 + minute * 60 + second - kTimezoneOffsetSeconds}};
}

inline std::optional<schedule::LocalTime> ParseLocalTime(const std::string& text) {
    int hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d", &hour, &minute, &second) < 2) return std::nullopt;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return std::nullopt;
    return schedule::LocalTime{hour, minute, second};
}

inline std::optional<schedule::LocalDate> ParseLocalDate(const std::string& text) {
    int year = 0, month = 0, day = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d", &year, &month, &day) != 3) return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    return schedule::LocalDate{year, month, day};
}

inline std::string FormatDate(const schedule::LocalDate& value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", value.year, value.month, value.day);
    return buffer;
}

inline std::string FormatTime(const schedule::LocalTime& value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", value.hour, value.minute, value.second);
    return buffer;
}

inline const char* StatusName(schedule::ScheduleStatus status) {
    switch (status) {
        case schedule::ScheduleStatus::kActive:
            return "active";
        case schedule::ScheduleStatus::kCancelled:
            return "cancelled";
        case schedule::ScheduleStatus::kCompleted:
            return "completed";
    }
    return "active";
}

inline const char* FrequencyName(schedule::Frequency value) {
    switch (value) {
        case schedule::Frequency::kDaily:
            return "daily";
        case schedule::Frequency::kWeekly:
            return "weekly";
        case schedule::Frequency::kMonthly:
            return "monthly";
        case schedule::Frequency::kYearly:
            return "yearly";
    }
    return "daily";
}

inline const char* MonthlyModeName(schedule::MonthlyMode value) {
    return value == schedule::MonthlyMode::kLastDay ? "last_day" : "specific_day";
}

inline ToolOutputValue RepeatOutput(const schedule::ScheduleRule& rule) {
    return ToolOutputValue::Object({
        MakeToolOutput("freq_type", ToolOutputValue::String(FrequencyName(rule.freq_type))),
        MakeToolOutput("interval_val", ToolOutputValue::Integer(rule.interval_val)),
        MakeToolOutput("start_date", ToolOutputValue::String(FormatDate(rule.start_date))),
        MakeToolOutput("start_time", ToolOutputValue::String(FormatTime(rule.start_time))),
        MakeToolOutput("end_time", rule.end_time.has_value() ? ToolOutputValue::String(FormatTime(*rule.end_time))
                                                             : ToolOutputValue::Null()),
        MakeToolOutput("end_date", rule.end_date.has_value() ? ToolOutputValue::String(FormatDate(*rule.end_date))
                                                             : ToolOutputValue::Null()),
        MakeToolOutput("occurrence_count", rule.occurrence_count.has_value()
                                               ? ToolOutputValue::Integer(*rule.occurrence_count)
                                               : ToolOutputValue::Null()),
        MakeToolOutput("weekdays_mask", rule.weekdays_mask.has_value() ? ToolOutputValue::Integer(*rule.weekdays_mask)
                                                                       : ToolOutputValue::Null()),
        MakeToolOutput("day_of_month", rule.day_of_month.has_value() ? ToolOutputValue::Integer(*rule.day_of_month)
                                                                     : ToolOutputValue::Null()),
        MakeToolOutput("month_of_year", rule.month_of_year.has_value() ? ToolOutputValue::Integer(*rule.month_of_year)
                                                                       : ToolOutputValue::Null()),
        MakeToolOutput("monthly_mode", rule.monthly_mode.has_value()
                                           ? ToolOutputValue::String(MonthlyModeName(*rule.monthly_mode))
                                           : ToolOutputValue::Null()),
    });
}

inline ToolOutputValue ScheduleOutput(const schedule::Schedule& value, const schedule::ScheduleRule* rule = nullptr) {
    return ToolOutputValue::Object({
        MakeToolOutput("id", ToolOutputValue::Integer(value.id)),
        MakeToolOutput("event", ToolOutputValue::String(value.event)),
        MakeToolOutput("status", ToolOutputValue::String(StatusName(value.status))),
        MakeToolOutput("start_time", value.start_time.has_value()
                                         ? ToolOutputValue::String(FormatDateTime(*value.start_time))
                                         : ToolOutputValue::Null()),
        MakeToolOutput("end_time", value.end_time.has_value() ? ToolOutputValue::String(FormatDateTime(*value.end_time))
                                                              : ToolOutputValue::Null()),
        MakeToolOutput("location",
                       value.location.has_value() ? ToolOutputValue::String(*value.location) : ToolOutputValue::Null()),
        MakeToolOutput("notes",
                       value.notes.has_value() ? ToolOutputValue::String(*value.notes) : ToolOutputValue::Null()),
        MakeToolOutput("rule_id",
                       value.rule_id.has_value() ? ToolOutputValue::Integer(*value.rule_id) : ToolOutputValue::Null()),
        MakeToolOutput("repeat", rule == nullptr ? ToolOutputValue::Null() : RepeatOutput(*rule)),
    });
}

inline ToolOutputArray ScheduleArrayOutput(const std::vector<schedule::Schedule>& schedules) {
    ToolOutputArray output;
    output.reserve(schedules.size());
    for (const auto& schedule : schedules) {
        output.emplace_back(MakeToolOutput(ScheduleOutput(schedule)));
    }
    return output;
}

inline ToolOutputValue FutureOccurrenceOutput(const schedule::ScheduleRule& rule, schedule::DateTime occurrence) {
    std::optional<schedule::DateTime> end_time;
    if (rule.end_time.has_value()) {
        const std::int64_t duration =
            schedule::LocalTimeToSeconds(*rule.end_time) - schedule::LocalTimeToSeconds(rule.start_time);
        end_time = occurrence + std::chrono::seconds{duration};
    }
    return ToolOutputValue::Object({
        MakeToolOutput("rule_id", ToolOutputValue::Integer(rule.id)),
        MakeToolOutput("original_start_time", ToolOutputValue::String(FormatDateTime(occurrence))),
        MakeToolOutput("event", ToolOutputValue::String(rule.event)),
        MakeToolOutput("status", ToolOutputValue::String("active")),
        MakeToolOutput("start_time", ToolOutputValue::String(FormatDateTime(occurrence))),
        MakeToolOutput("end_time", end_time.has_value() ? ToolOutputValue::String(FormatDateTime(*end_time))
                                                        : ToolOutputValue::Null()),
        MakeToolOutput("location",
                       rule.location.has_value() ? ToolOutputValue::String(*rule.location) : ToolOutputValue::Null()),
        MakeToolOutput("notes",
                       rule.notes.has_value() ? ToolOutputValue::String(*rule.notes) : ToolOutputValue::Null()),
        MakeToolOutput("repeat", RepeatOutput(rule)),
    });
}

inline ToolOutputArray FutureOccurrencesOutput(const schedule::ScheduleRule& rule,
                                               const std::vector<schedule::DateTime>& occurrences) {
    ToolOutputArray output;
    output.reserve(occurrences.size());
    for (const auto& occurrence : occurrences) {
        output.emplace_back(MakeToolOutput(FutureOccurrenceOutput(rule, occurrence)));
    }
    return output;
}

inline ToolOutputValue RuleOutput(const schedule::ScheduleRule& rule) {
    return ToolOutputValue::Object({
        MakeToolOutput("id", ToolOutputValue::Integer(rule.id)),
        MakeToolOutput("event", ToolOutputValue::String(rule.event)),
        MakeToolOutput("status", ToolOutputValue::String(StatusName(rule.status))),
        MakeToolOutput("freq_type", ToolOutputValue::String(FrequencyName(rule.freq_type))),
        MakeToolOutput("interval_val", ToolOutputValue::Integer(rule.interval_val)),
        MakeToolOutput("start_date", ToolOutputValue::String(FormatDate(rule.start_date))),
        MakeToolOutput("start_time", ToolOutputValue::String(FormatTime(rule.start_time))),
        MakeToolOutput("end_time", rule.end_time.has_value() ? ToolOutputValue::String(FormatTime(*rule.end_time))
                                                             : ToolOutputValue::Null()),
        MakeToolOutput("end_date", rule.end_date.has_value() ? ToolOutputValue::String(FormatDate(*rule.end_date))
                                                             : ToolOutputValue::Null()),
        MakeToolOutput("occurrence_count", rule.occurrence_count.has_value()
                                               ? ToolOutputValue::Integer(*rule.occurrence_count)
                                               : ToolOutputValue::Null()),
        MakeToolOutput("weekdays_mask", rule.weekdays_mask.has_value() ? ToolOutputValue::Integer(*rule.weekdays_mask)
                                                                       : ToolOutputValue::Null()),
        MakeToolOutput("day_of_month", rule.day_of_month.has_value() ? ToolOutputValue::Integer(*rule.day_of_month)
                                                                     : ToolOutputValue::Null()),
        MakeToolOutput("month_of_year", rule.month_of_year.has_value() ? ToolOutputValue::Integer(*rule.month_of_year)
                                                                       : ToolOutputValue::Null()),
        MakeToolOutput("monthly_mode", rule.monthly_mode.has_value()
                                           ? ToolOutputValue::String(MonthlyModeName(*rule.monthly_mode))
                                           : ToolOutputValue::Null()),
    });
}

inline ToolOutputValue ExceptionOutput(const schedule::ScheduleException& exception) {
    return ToolOutputValue::Object({
        MakeToolOutput("id", ToolOutputValue::Integer(exception.id)),
        MakeToolOutput("rule_id", ToolOutputValue::Integer(exception.rule_id)),
        MakeToolOutput("original_start_time", ToolOutputValue::String(FormatDateTime(exception.original_start_time))),
        MakeToolOutput("type",
                       ToolOutputValue::String(exception.type == schedule::ExceptionType::kSkip ? "skip" : "modify")),
        MakeToolOutput("schedule_id", exception.schedule_id.has_value()
                                          ? ToolOutputValue::Integer(*exception.schedule_id)
                                          : ToolOutputValue::Null()),
        MakeToolOutput("override_start_time",
                       exception.override_start_time.has_value()
                           ? ToolOutputValue::String(FormatDateTime(*exception.override_start_time))
                           : ToolOutputValue::Null()),
        MakeToolOutput("override_end_time", exception.override_end_time.has_value()
                                                ? ToolOutputValue::String(FormatDateTime(*exception.override_end_time))
                                                : ToolOutputValue::Null()),
        MakeToolOutput("override_event", exception.override_event.has_value()
                                             ? ToolOutputValue::String(*exception.override_event)
                                             : ToolOutputValue::Null()),
        MakeToolOutput("override_location", exception.override_location.has_value()
                                                ? ToolOutputValue::String(*exception.override_location)
                                                : ToolOutputValue::Null()),
        MakeToolOutput("override_notes", exception.override_notes.has_value()
                                             ? ToolOutputValue::String(*exception.override_notes)
                                             : ToolOutputValue::Null()),
    });
}

inline ToolOutputArray ExceptionsOutput(const std::vector<schedule::ScheduleException>& exceptions) {
    ToolOutputArray output;
    output.reserve(exceptions.size());
    for (const auto& exception : exceptions) {
        output.emplace_back(MakeToolOutput(ExceptionOutput(exception)));
    }
    return output;
}

}  // namespace voicelife::mcp::schedule_tool_output
