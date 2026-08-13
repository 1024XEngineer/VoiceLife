#include "voicelife/mcp/schedule_rule_mcp_tools.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_rule_commands.h"
#include "voicelife/schedule/schedule_rule_results.h"
#include "voicelife/schedule/schedule_rule_service.h"

namespace voicelife::mcp {
namespace {

using schedule::DateTime;

ToolResult Failure(Status status) { return {.status = std::move(status), .output = {}}; }

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

const char* FrequencyName(schedule::Frequency value) {
    switch (value) {
        case schedule::Frequency::kDaily: return "daily";
        case schedule::Frequency::kWeekly: return "weekly";
        case schedule::Frequency::kMonthly: return "monthly";
        case schedule::Frequency::kYearly: return "yearly";
    }
    return "daily";
}

const char* MonthlyModeName(schedule::MonthlyMode value) {
    return value == schedule::MonthlyMode::kLastDay ? "last_day" : "specific_day";
}

std::string UnixTime(DateTime value) { return std::to_string(value.time_since_epoch().count()); }

void AddRuleOutput(const schedule::ScheduleRule& rule, ToolResult& result) {
    result.output["id"] = std::to_string(rule.id);
    result.output["event"] = rule.event;
    result.output["freq_type"] = FrequencyName(rule.freq_type);
    result.output["interval_val"] = std::to_string(rule.interval_val);
    result.output["start_time"] = FormatTime(rule.start_time);
    result.output["start_date"] = FormatDate(rule.start_date);
    result.output["status"] = std::to_string(static_cast<int>(rule.status));
    if (rule.location.has_value()) result.output["location"] = *rule.location;
    if (rule.notes.has_value()) result.output["notes"] = *rule.notes;
    if (rule.end_time.has_value()) result.output["end_time"] = FormatTime(*rule.end_time);
    if (rule.weekdays_mask.has_value()) result.output["weekdays_mask"] = std::to_string(*rule.weekdays_mask);
    if (rule.day_of_month.has_value()) result.output["day_of_month"] = std::to_string(*rule.day_of_month);
    if (rule.month_of_year.has_value()) result.output["month_of_year"] = std::to_string(*rule.month_of_year);
    if (rule.monthly_mode.has_value()) result.output["monthly_mode"] = MonthlyModeName(*rule.monthly_mode);
    if (rule.end_date.has_value()) result.output["end_date"] = FormatDate(*rule.end_date);
    if (rule.occurrence_count.has_value()) result.output["occurrence_count"] = std::to_string(*rule.occurrence_count);
}

void AddScheduleOutput(const schedule::Schedule& value, ToolResult& result) {
    result.output["id"] = std::to_string(value.id);
    result.output["event"] = value.event;
    result.output["status"] = std::to_string(static_cast<int>(value.status));
    if (value.start_time.has_value()) result.output["start_time"] = UnixTime(*value.start_time);
    if (value.end_time.has_value()) result.output["end_time"] = UnixTime(*value.end_time);
    if (value.location.has_value()) result.output["location"] = *value.location;
    if (value.notes.has_value()) result.output["notes"] = *value.notes;
    if (value.rule_id.has_value()) result.output["rule_id"] = std::to_string(*value.rule_id);
}

void AddExceptionOutput(const schedule::ScheduleException& exception, ToolResult& result) {
    result.output["id"] = std::to_string(exception.id);
    result.output["rule_id"] = std::to_string(exception.rule_id);
    result.output["original_start_time"] = UnixTime(exception.original_start_time);
    result.output["type"] = exception.type == schedule::ExceptionType::kSkip ? "skip" : "modify";
    if (exception.schedule_id.has_value()) result.output["schedule_id"] = std::to_string(*exception.schedule_id);
    if (exception.override_start_time.has_value()) result.output["override_start_time"] = UnixTime(*exception.override_start_time);
    if (exception.override_end_time.has_value()) result.output["override_end_time"] = UnixTime(*exception.override_end_time);
    if (exception.override_event.has_value()) result.output["override_event"] = *exception.override_event;
}

PropertyList CreateRuleProperties() {
    return PropertyList({
        Property("event", PropertyType::kString),
        Property("freq_type", PropertyType::kString),
        Property("start_time", PropertyType::kString),
        Property("start_date", PropertyType::kString),
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
        "schedule_rule.create", "创建周期日程规则并生成首条实例；时间用 HH:MM:SS，日期用 YYYY-MM-DD。",
        CreateRuleProperties(), [&service](const PropertyList& properties) {
            schedule::CreateScheduleRuleCommand command;
            command.event = properties.value<std::string>("event").value_or("");
            command.freq_type = ParseFrequency(properties.value<std::string>("freq_type").value_or(""))
                                    .value_or(schedule::Frequency::kDaily);
            const auto start_time = ParseLocalTime(properties.value<std::string>("start_time").value_or(""));
            const auto start_date = ParseLocalDate(properties.value<std::string>("start_date").value_or(""));
            if (!start_time.has_value() || !start_date.has_value()) {
                return Failure(Status::Error(ErrorCode::kInvalidArgument, "开始时间或日期格式无效"));
            }
            command.start_time = *start_time;
            command.start_date = *start_date;
            if (properties.value<std::string>("end_time").has_value()) {
                command.end_time = ParseLocalTime(*properties.value<std::string>("end_time"));
            }
            command.location = properties.value<std::string>("location");
            command.notes = properties.value<std::string>("notes");
            command.interval_val = static_cast<int32_t>(properties.value<int64_t>("interval_val").value_or(1));
            command.weekdays_mask = properties.value<int64_t>("weekdays_mask").has_value()
                                        ? std::optional<uint8_t>{static_cast<uint8_t>(*properties.value<int64_t>("weekdays_mask"))}
                                        : std::nullopt;
            if (properties.value<std::string>("monthly_mode").has_value()) {
                command.monthly_mode = ParseMonthlyMode(*properties.value<std::string>("monthly_mode"));
            }
            command.day_of_month = properties.value<int64_t>("day_of_month").has_value()
                                       ? std::optional<uint8_t>{static_cast<uint8_t>(*properties.value<int64_t>("day_of_month"))}
                                       : std::nullopt;
            command.month_of_year = properties.value<int64_t>("month_of_year").has_value()
                                        ? std::optional<uint8_t>{static_cast<uint8_t>(*properties.value<int64_t>("month_of_year"))}
                                        : std::nullopt;
            if (properties.value<std::string>("end_date").has_value()) {
                command.end_date = ParseLocalDate(*properties.value<std::string>("end_date"));
            }
            command.occurrence_count = properties.value<int64_t>("occurrence_count").has_value()
                                           ? std::optional<int32_t>{static_cast<int32_t>(*properties.value<int64_t>("occurrence_count"))}
                                           : std::nullopt;
            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);

            const auto result = service.create_schedule_rule(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolResult output{.status = result.status, .output = {}};
            if (result.rule.has_value()) AddRuleOutput(*result.rule, output);
            output.output["instance_count"] = std::to_string(result.schedules.size());
            output.output["conflict_count"] = std::to_string(result.conflicts.size());
            return output;
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
            ToolResult output{.status = result.status, .output = {{"total", std::to_string(result.total)}}};
            output.output["count"] = std::to_string(result.rules.size());
            for (std::size_t i = 0; i < result.rules.size(); ++i) {
                const auto& view = result.rules[i];
                const std::string prefix = "rule_" + std::to_string(i);
                ToolResult item{.status = Status::Ok(), .output = {}};
                AddRuleOutput(view.rule, item);
                item.output["exception_count"] = std::to_string(view.exceptions.size());
                item.output["upcoming_count"] = std::to_string(view.upcoming_occurrences.size());
                for (std::size_t j = 0; j < view.upcoming_occurrences.size(); ++j) {
                    item.output["upcoming_" + std::to_string(j)] = UnixTime(view.upcoming_occurrences[j]);
                }
                for (const auto& [key, value] : item.output) output.output[prefix + "_" + key] = value;
            }
            return output;
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule_occurrence.skip", "跳过周期规则中的某一次；original_start_time 用 Unix 秒。",
        PropertyList({Property("rule_id", PropertyType::kInteger),
                      Property("original_start_time", PropertyType::kInteger)}),
        [&service](const PropertyList& properties) {
            schedule::SkipScheduleOccurrenceCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            command.original_start_time =
                schedule::DateTime{std::chrono::seconds{properties.value<int64_t>("original_start_time").value_or(0)}};
            const auto result = service.skip_schedule_occurrence(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolResult output{.status = result.status, .output = {}};
            if (result.exception.has_value()) AddExceptionOutput(*result.exception, output);
            return output;
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
            Property::Optional("start_date", PropertyType::kString),
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
                command.weekdays_mask =
                    static_cast<uint8_t>(*properties.value<int64_t>("weekdays_mask"));
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
            if (properties.value<std::string>("start_date").has_value()) {
                command.start_date = ParseLocalDate(*properties.value<std::string>("start_date"));
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
            ToolResult output{.status = result.status, .output = {}};
            if (result.rule.has_value()) AddRuleOutput(*result.rule, output);
            output.output["instance_count"] = std::to_string(result.schedules.size());
            return output;
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule_rule.cancel", "取消整条周期规则及其未来实例。",
        PropertyList({Property("rule_id", PropertyType::kInteger)}),
        [&service](const PropertyList& properties) {
            schedule::CancelScheduleRuleCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            const auto result = service.cancel_schedule_rule(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolResult output{.status = result.status, .output = {}};
            if (result.rule.has_value()) AddRuleOutput(*result.rule, output);
            output.output["cancelled_count"] = std::to_string(result.cancelled_count);
            return output;
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
                command.start_time =
                    schedule::DateTime{std::chrono::seconds{*properties.value<int64_t>("start_time")}};
            }
            if (properties.value<int64_t>("end_time").has_value()) {
                command.end_time =
                    schedule::DateTime{std::chrono::seconds{*properties.value<int64_t>("end_time")}};
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
            ToolResult output{.status = result.status, .output = {}};
            if (result.schedule.has_value()) AddScheduleOutput(*result.schedule, output);
            if (result.exception.has_value()) AddExceptionOutput(*result.exception, output);
            return output;
        });
    if (!status.ok()) return status;

    return server.add_tool(
        "schedule_rule.generate_next", "生成某周期规则的下一条实例。",
        PropertyList({Property("rule_id", PropertyType::kInteger)}),
        [&service](const PropertyList& properties) {
            schedule::GenerateNextScheduleInstanceCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            const auto result = service.generate_next_schedule_instance(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolResult output{.status = result.status, .output = {}};
            if (result.schedule.has_value()) AddScheduleOutput(*result.schedule, output);
            return output;
        });
}

}  // namespace voicelife::mcp
