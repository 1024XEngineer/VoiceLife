#include "voicelife/mcp/schedule_rule_mcp_tools.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "schedule_tool_output.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_rule_commands.h"
#include "voicelife/schedule/schedule_rule_results.h"
#include "voicelife/schedule/schedule_rule_service.h"

namespace voicelife::mcp {
namespace {

using schedule::DateTime;
using voicelife::MakeToolOutput;
using voicelife::ToolOutputArray;
using voicelife::ToolOutputObject;
using voicelife::ToolOutputValue;

ToolResult Failure(Status status) { return ToolResult::Failure(std::move(status)); }

std::optional<schedule::LocalTime> ParseLocalTime(const std::string& text) {
    int hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d", &hour, &minute, &second) < 2) return std::nullopt;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return std::nullopt;
    return schedule::LocalTime{hour, minute, second};
}

std::optional<schedule::LocalDate> ParseLocalDate(const std::string& text) {
    int year = 0, month = 0, day = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d", &year, &month, &day) != 3) return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    return schedule::LocalDate{year, month, day};
}

std::optional<schedule::Frequency> ParseFrequency(const std::string& text) {
    if (text == "daily") return schedule::Frequency::kDaily;
    if (text == "weekly") return schedule::Frequency::kWeekly;
    if (text == "monthly") return schedule::Frequency::kMonthly;
    if (text == "yearly") return schedule::Frequency::kYearly;
    return std::nullopt;
}

std::optional<schedule::MonthlyMode> ParseMonthlyMode(const std::string& text) {
    if (text == "specific_day") return schedule::MonthlyMode::kSpecificDay;
    if (text == "last_day") return schedule::MonthlyMode::kLastDay;
    return std::nullopt;
}

std::string FormatTime(const schedule::LocalTime& value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", value.hour, value.minute, value.second);
    return buffer;
}

std::string FormatDate(const schedule::LocalDate& value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", value.year, value.month, value.day);
    return buffer;
}

int64_t UnixTime(schedule::DateTime value) { return value.time_since_epoch().count(); }

const char* FrequencyName(schedule::Frequency value) {
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

const char* MonthlyModeName(schedule::MonthlyMode value) {
    return value == schedule::MonthlyMode::kLastDay ? "last_day" : "specific_day";
}

ToolOutputValue RuleOutput(const schedule::ScheduleRule& rule) {
    ToolOutputObject fields = {
        MakeToolOutput("id", ToolOutputValue::Integer(rule.id)),
        MakeToolOutput("event", ToolOutputValue::String(rule.event)),
        MakeToolOutput("freq_type", ToolOutputValue::String(FrequencyName(rule.freq_type))),
        MakeToolOutput("interval_val", ToolOutputValue::Integer(rule.interval_val)),
        MakeToolOutput("start_time", ToolOutputValue::String(FormatTime(rule.start_time))),
        MakeToolOutput("start_date", ToolOutputValue::String(FormatDate(rule.start_date))),
        MakeToolOutput("status", ToolOutputValue::Integer(static_cast<int>(rule.status))),
    };
    if (rule.location.has_value())
        fields.emplace_back(MakeToolOutput("location", ToolOutputValue::String(*rule.location)));
    if (rule.notes.has_value()) fields.emplace_back(MakeToolOutput("notes", ToolOutputValue::String(*rule.notes)));
    if (rule.end_time.has_value())
        fields.emplace_back(MakeToolOutput("end_time", ToolOutputValue::String(FormatTime(*rule.end_time))));
    if (rule.weekdays_mask.has_value())
        fields.emplace_back(MakeToolOutput("weekdays_mask", ToolOutputValue::Integer(*rule.weekdays_mask)));
    if (rule.day_of_month.has_value())
        fields.emplace_back(MakeToolOutput("day_of_month", ToolOutputValue::Integer(*rule.day_of_month)));
    if (rule.month_of_year.has_value())
        fields.emplace_back(MakeToolOutput("month_of_year", ToolOutputValue::Integer(*rule.month_of_year)));
    if (rule.monthly_mode.has_value())
        fields.emplace_back(
            MakeToolOutput("monthly_mode", ToolOutputValue::String(MonthlyModeName(*rule.monthly_mode))));
    if (rule.end_date.has_value())
        fields.emplace_back(MakeToolOutput("end_date", ToolOutputValue::String(FormatDate(*rule.end_date))));
    if (rule.occurrence_count.has_value())
        fields.emplace_back(MakeToolOutput("occurrence_count", ToolOutputValue::Integer(*rule.occurrence_count)));
    return ToolOutputValue::Object(std::move(fields));
}

ToolOutputValue ExceptionOutput(const schedule::ScheduleException& exception) {
    ToolOutputObject fields = {
        MakeToolOutput("id", ToolOutputValue::Integer(exception.id)),
        MakeToolOutput("rule_id", ToolOutputValue::Integer(exception.rule_id)),
        MakeToolOutput("original_start_time", ToolOutputValue::Integer(UnixTime(exception.original_start_time))),
        MakeToolOutput("type",
                       ToolOutputValue::String(exception.type == schedule::ExceptionType::kSkip ? "skip" : "modify")),
    };
    if (exception.schedule_id.has_value())
        fields.emplace_back(MakeToolOutput("schedule_id", ToolOutputValue::Integer(*exception.schedule_id)));
    if (exception.override_start_time.has_value())
        fields.emplace_back(
            MakeToolOutput("override_start_time", ToolOutputValue::Integer(UnixTime(*exception.override_start_time))));
    if (exception.override_end_time.has_value())
        fields.emplace_back(
            MakeToolOutput("override_end_time", ToolOutputValue::Integer(UnixTime(*exception.override_end_time))));
    if (exception.override_event.has_value())
        fields.emplace_back(MakeToolOutput("override_event", ToolOutputValue::String(*exception.override_event)));
    return ToolOutputValue::Object(std::move(fields));
}

ToolOutputArray ExceptionArrayOutput(const std::vector<schedule::ScheduleException>& exceptions) {
    ToolOutputArray output;
    output.reserve(exceptions.size());
    for (const auto& exception : exceptions) {
        output.emplace_back(MakeToolOutput(ExceptionOutput(exception)));
    }
    return output;
}

ToolOutputArray DateTimeArrayOutput(const std::vector<schedule::DateTime>& values) {
    ToolOutputArray output;
    output.reserve(values.size());
    for (const auto& value : values) {
        output.emplace_back(MakeToolOutput(ToolOutputValue::Integer(UnixTime(value))));
    }
    return output;
}

PropertyList CreateRuleProperties() {
    return PropertyList({
        Property("event", PropertyType::kString),
        Property("freq_type", PropertyType::kString),
        Property("start_time", PropertyType::kString),
        Property::Optional("end_time", PropertyType::kString),
        Property::Optional("location", PropertyType::kString),
        Property::Optional("notes", PropertyType::kString),
        Property("interval_val", PropertyType::kInteger, int64_t{1}),
        Property::Optional("weekdays_mask", PropertyType::kInteger),
        Property::Optional("monthly_mode", PropertyType::kString),
        Property::Optional("day_of_month", PropertyType::kInteger),
        Property::Optional("month_of_year", PropertyType::kInteger),
        Property::Optional("end_date", PropertyType::kString),
        Property::Optional("occurrence_count", PropertyType::kInteger),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false}),
    });
}

PropertyList QueryRulesProperties() {
    return PropertyList({
        Property::Optional("rule_id", PropertyType::kInteger),
        Property::Optional("keyword", PropertyType::kString),
        Property("status", PropertyType::kString, std::string("active")),
        Property("limit", PropertyType::kInteger, int64_t{10}),
        Property("offset", PropertyType::kInteger, int64_t{0}),
    });
}

}  // namespace

Status RegisterScheduleRuleMcpTools(McpServer& server, schedule::ScheduleRuleService& service) {
    Status status = server.add_tool(
        "schedule_rule.create", "创建周期日程规则并生成首条实例；首个发生日期由服务端计算。", CreateRuleProperties(),
        [&service](const PropertyList& properties) {
            schedule::CreateScheduleRuleCommand command;
            command.event = properties.value<std::string>("event").value_or("");
            command.freq_type = ParseFrequency(properties.value<std::string>("freq_type").value_or(""))
                                    .value_or(schedule::Frequency::kDaily);
            const auto start_time = ParseLocalTime(properties.value<std::string>("start_time").value_or(""));
            if (!start_time.has_value()) {
                return Failure(Status::Error(ErrorCode::kInvalidArgument, "开始时间格式无效"));
            }
            command.start_time = *start_time;
            if (properties.value<std::string>("end_time").has_value()) {
                command.end_time = ParseLocalTime(*properties.value<std::string>("end_time"));
            }
            command.location = properties.value<std::string>("location");
            command.notes = properties.value<std::string>("notes");
            command.interval_val = static_cast<int32_t>(properties.value<int64_t>("interval_val").value_or(1));
            command.weekdays_mask =
                properties.value<int64_t>("weekdays_mask").has_value()
                    ? std::optional<uint8_t>{static_cast<uint8_t>(*properties.value<int64_t>("weekdays_mask"))}
                    : std::nullopt;
            if (properties.value<std::string>("monthly_mode").has_value()) {
                command.monthly_mode = ParseMonthlyMode(*properties.value<std::string>("monthly_mode"));
            }
            command.day_of_month =
                properties.value<int64_t>("day_of_month").has_value()
                    ? std::optional<uint8_t>{static_cast<uint8_t>(*properties.value<int64_t>("day_of_month"))}
                    : std::nullopt;
            command.month_of_year =
                properties.value<int64_t>("month_of_year").has_value()
                    ? std::optional<uint8_t>{static_cast<uint8_t>(*properties.value<int64_t>("month_of_year"))}
                    : std::nullopt;
            if (properties.value<std::string>("end_date").has_value()) {
                command.end_date = ParseLocalDate(*properties.value<std::string>("end_date"));
            }
            command.occurrence_count =
                properties.value<int64_t>("occurrence_count").has_value()
                    ? std::optional<int32_t>{static_cast<int32_t>(*properties.value<int64_t>("occurrence_count"))}
                    : std::nullopt;
            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);

            const auto result = service.create_schedule_rule(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolOutputObject fields;
            if (result.rule.has_value()) fields.emplace_back(MakeToolOutput("rule", RuleOutput(*result.rule)));
            fields.emplace_back(MakeToolOutput(
                "instances", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.schedules))));
            fields.emplace_back(MakeToolOutput(
                "conflicts", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))));
            return ToolResult::Success(ToolOutputValue::Object(std::move(fields)));
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule_rule.query", "查询周期规则及其例外与未来发生时间。", QueryRulesProperties(),
        [&service](const PropertyList& properties) {
            schedule::QueryScheduleRulesCommand command;
            command.rule_id = properties.value<int64_t>("rule_id");
            command.keyword = properties.value<std::string>("keyword");
            command.status = properties.value<std::string>("status").value_or("active") == "all"
                                 ? schedule::ScheduleStatusFilter::kAll
                                 : schedule::ScheduleStatusFilter::kActive;
            command.limit = properties.value<int64_t>("limit").value_or(10);
            command.offset = properties.value<int64_t>("offset").value_or(0);
            const auto result = service.query_schedule_rules(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolOutputArray rules;
            rules.reserve(result.rules.size());
            for (const auto& view : result.rules) {
                ToolOutputObject item_fields = {
                    MakeToolOutput("rule", RuleOutput(view.rule)),
                    MakeToolOutput("exceptions", ToolOutputValue::Array(ExceptionArrayOutput(view.exceptions))),
                    MakeToolOutput("upcoming_occurrences",
                                   ToolOutputValue::Array(DateTimeArrayOutput(view.upcoming_occurrences))),
                };
                rules.emplace_back(MakeToolOutput(ToolOutputValue::Object(std::move(item_fields))));
            }
            return ToolResult::Success(ToolOutputValue::Object({
                MakeToolOutput("total", ToolOutputValue::Integer(result.total)),
                MakeToolOutput("rules", ToolOutputValue::Array(std::move(rules))),
            }));
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule_occurrence.skip", "跳过周期规则中的某一次；original_start_time 用 Unix 秒。",
        PropertyList(
            {Property("rule_id", PropertyType::kInteger), Property("original_start_time", PropertyType::kInteger)}),
        [&service](const PropertyList& properties) {
            schedule::SkipScheduleOccurrenceCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            command.original_start_time =
                schedule::DateTime{std::chrono::seconds{properties.value<int64_t>("original_start_time").value_or(0)}};
            const auto result = service.skip_schedule_occurrence(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolOutputObject fields;
            if (result.schedule.has_value()) {
                fields.emplace_back(MakeToolOutput("schedule", schedule_tool_output::ScheduleOutput(*result.schedule)));
            }
            if (result.exception.has_value()) {
                fields.emplace_back(MakeToolOutput("exception", ExceptionOutput(*result.exception)));
            }
            return ToolResult::Success(ToolOutputValue::Object(std::move(fields)));
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule_rule.update", "修改整条周期规则并重建未来实例。",
        PropertyList({
            Property("rule_id", PropertyType::kInteger),
            Property::Optional("event", PropertyType::kString),
            Property::Optional("location", PropertyType::kString),
            Property::Optional("notes", PropertyType::kString),
            Property::Optional("freq_type", PropertyType::kString),
            Property::Optional("interval_val", PropertyType::kInteger),
            Property::Optional("weekdays_mask", PropertyType::kInteger),
            Property::Optional("monthly_mode", PropertyType::kString),
            Property::Optional("day_of_month", PropertyType::kInteger),
            Property::Optional("month_of_year", PropertyType::kInteger),
            Property::Optional("start_time", PropertyType::kString),
            Property::Optional("end_time", PropertyType::kString),
            Property::Optional("end_date", PropertyType::kString),
            Property::Optional("occurrence_count", PropertyType::kInteger),
            Property("ignore_conflict", PropertyType::kBoolean, bool{false}),
        }),
        [&service](const PropertyList& properties) {
            schedule::UpdateScheduleRuleCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            command.event = properties.value<std::string>("event");
            if (properties.value<std::string>("location").has_value()) {
                command.location = *properties.value<std::string>("location");
            }
            if (properties.value<std::string>("notes").has_value()) {
                command.notes = *properties.value<std::string>("notes");
            }
            if (properties.value<std::string>("freq_type").has_value()) {
                command.freq_type = ParseFrequency(*properties.value<std::string>("freq_type"));
            }
            if (properties.value<int64_t>("interval_val").has_value()) {
                command.interval_val = static_cast<int32_t>(*properties.value<int64_t>("interval_val"));
            }
            if (properties.value<int64_t>("weekdays_mask").has_value()) {
                command.weekdays_mask = static_cast<uint8_t>(*properties.value<int64_t>("weekdays_mask"));
            }
            if (properties.value<std::string>("monthly_mode").has_value()) {
                command.monthly_mode = ParseMonthlyMode(*properties.value<std::string>("monthly_mode"));
            }
            if (properties.value<int64_t>("day_of_month").has_value()) {
                command.day_of_month = static_cast<uint8_t>(*properties.value<int64_t>("day_of_month"));
            }
            if (properties.value<int64_t>("month_of_year").has_value()) {
                command.month_of_year = static_cast<uint8_t>(*properties.value<int64_t>("month_of_year"));
            }
            if (properties.value<std::string>("start_time").has_value()) {
                command.start_time = ParseLocalTime(*properties.value<std::string>("start_time"));
            }
            if (properties.value<std::string>("end_time").has_value()) {
                command.end_time = ParseLocalTime(*properties.value<std::string>("end_time"));
            }
            if (properties.value<std::string>("end_date").has_value()) {
                command.end_date = ParseLocalDate(*properties.value<std::string>("end_date"));
            }
            if (properties.value<int64_t>("occurrence_count").has_value()) {
                command.occurrence_count = static_cast<int32_t>(*properties.value<int64_t>("occurrence_count"));
            }
            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);

            const auto result = service.update_schedule_rule(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolOutputObject fields;
            if (result.rule.has_value()) fields.emplace_back(MakeToolOutput("rule", RuleOutput(*result.rule)));
            fields.emplace_back(MakeToolOutput(
                "instances", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.schedules))));
            fields.emplace_back(MakeToolOutput(
                "conflicts", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))));
            return ToolResult::Success(ToolOutputValue::Object(std::move(fields)));
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule_rule.cancel", "取消整条周期规则及其未来实例。",
        PropertyList({Property("rule_id", PropertyType::kInteger)}), [&service](const PropertyList& properties) {
            schedule::CancelScheduleRuleCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            const auto result = service.cancel_schedule_rule(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolOutputObject fields = {
                MakeToolOutput("cancelled_count", ToolOutputValue::Integer(result.cancelled_count)),
            };
            if (result.rule.has_value()) fields.emplace_back(MakeToolOutput("rule", RuleOutput(*result.rule)));
            return ToolResult::Success(ToolOutputValue::Object(std::move(fields)));
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule_occurrence.update", "修改周期中的某一次；original_start_time 与时间用 Unix 秒。",
        PropertyList({
            Property("rule_id", PropertyType::kInteger),
            Property("original_start_time", PropertyType::kInteger),
            Property::Optional("event", PropertyType::kString),
            Property::Optional("start_time", PropertyType::kInteger),
            Property::Optional("end_time", PropertyType::kInteger),
            Property::Optional("location", PropertyType::kString),
            Property::Optional("notes", PropertyType::kString),
            Property("ignore_conflict", PropertyType::kBoolean, bool{false}),
        }),
        [&service](const PropertyList& properties) {
            schedule::UpdateScheduleOccurrenceCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            command.original_start_time =
                schedule::DateTime{std::chrono::seconds{properties.value<int64_t>("original_start_time").value_or(0)}};
            if (properties.value<std::string>("event").has_value()) {
                command.event = *properties.value<std::string>("event");
            }
            if (properties.value<int64_t>("start_time").has_value()) {
                command.start_time = schedule::DateTime{std::chrono::seconds{*properties.value<int64_t>("start_time")}};
            }
            if (properties.value<int64_t>("end_time").has_value()) {
                command.end_time = schedule::DateTime{std::chrono::seconds{*properties.value<int64_t>("end_time")}};
            }
            if (properties.value<std::string>("location").has_value()) {
                command.location = *properties.value<std::string>("location");
            }
            if (properties.value<std::string>("notes").has_value()) {
                command.notes = *properties.value<std::string>("notes");
            }
            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);

            const auto result = service.update_schedule_occurrence(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolOutputObject fields;
            if (result.schedule.has_value()) {
                fields.emplace_back(MakeToolOutput("schedule", schedule_tool_output::ScheduleOutput(*result.schedule)));
            }
            if (result.exception.has_value()) {
                fields.emplace_back(MakeToolOutput("exception", ExceptionOutput(*result.exception)));
            }
            fields.emplace_back(MakeToolOutput(
                "conflicts", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))));
            return ToolResult::Success(ToolOutputValue::Object(std::move(fields)));
        });
    if (!status.ok()) return status;

    return server.add_tool(
        "schedule_rule.generate_next", "生成某周期规则的下一条实例。",
        PropertyList({Property("rule_id", PropertyType::kInteger)}), [&service](const PropertyList& properties) {
            schedule::GenerateNextScheduleInstanceCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            const auto result = service.generate_next_schedule_instance(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolOutputObject fields;
            if (result.schedule.has_value()) {
                fields.emplace_back(MakeToolOutput("schedule", schedule_tool_output::ScheduleOutput(*result.schedule)));
            }
            return ToolResult::Success(ToolOutputValue::Object(std::move(fields)));
        });
}

}  // namespace voicelife::mcp
