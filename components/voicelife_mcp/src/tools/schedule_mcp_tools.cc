#include "voicelife/mcp/schedule_mcp_tools.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "schedule_tool_output.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"
#include "voicelife/schedule/schedule_rule_commands.h"
#include "voicelife/schedule/schedule_rule_results.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::mcp {
namespace {

using schedule::DateTime;
using schedule::ScheduleRule;
using schedule::ScheduleRuleService;
using schedule::ScheduleService;
using voicelife::MakeToolOutput;
using voicelife::ToolOutputArray;
using voicelife::ToolOutputObject;
using voicelife::ToolOutputValue;

ToolResult Output(ToolOutputObject fields) { return ToolResult::Success(ToolOutputValue::Object(std::move(fields))); }

ToolResult FailureOutput(std::string message) {
    return Output({
        MakeToolOutput("status", ToolOutputValue::String("failure")),
        MakeToolOutput("message", ToolOutputValue::String(std::move(message))),
    });
}

ToolResult ConflictOutput(std::string message, ToolOutputArray conflicts) {
    return Output({
        MakeToolOutput("status", ToolOutputValue::String("conflict")),
        MakeToolOutput("message", ToolOutputValue::String(std::move(message))),
        MakeToolOutput("conflicts", ToolOutputValue::Array(std::move(conflicts))),
    });
}

schedule::ScheduleStatusFilter ParseStatus(const std::string& value) {
    if (value == "all") return schedule::ScheduleStatusFilter::kAll;
    if (value == "active") return schedule::ScheduleStatusFilter::kActive;
    if (value == "cancelled") return schedule::ScheduleStatusFilter::kCancelled;
    if (value == "completed") return schedule::ScheduleStatusFilter::kCompleted;
    return schedule::ScheduleStatusFilter::kActive;
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

std::optional<std::string> JsonString(const JsonValue& object, const std::string& key) {
    const JsonValue* value = object.Get(key);
    return value != nullptr && value->IsString() ? std::optional<std::string>{value->string} : std::nullopt;
}

std::optional<int64_t> JsonInteger(const JsonValue& object, const std::string& key) {
    const JsonValue* value = object.Get(key);
    if (value == nullptr || value->kind != JsonValue::Kind::kNumber ||
        value->number != static_cast<int64_t>(value->number)) {
        return std::nullopt;
    }
    return static_cast<int64_t>(value->number);
}

struct ParsedRepeat {
    std::optional<schedule::Frequency> freq_type;
    std::optional<schedule::LocalTime> start_time;
    std::optional<schedule::LocalTime> end_time;
    std::optional<schedule::LocalDate> start_date;
    std::optional<schedule::LocalDate> end_date;
    std::optional<int32_t> interval_val;
    std::optional<uint8_t> weekdays_mask;
    std::optional<uint8_t> day_of_month;
    std::optional<uint8_t> month_of_year;
    std::optional<schedule::MonthlyMode> monthly_mode;
    std::optional<int32_t> occurrence_count;
    std::string error;

    [[nodiscard]] bool ok() const { return error.empty(); }
};

ParsedRepeat ParseRepeat(const std::optional<JsonValue>& repeat, bool require_anchor) {
    ParsedRepeat parsed;
    if (!repeat.has_value()) return parsed;
    if (!repeat->IsObject()) {
        parsed.error = "repeat 必须是对象";
        return parsed;
    }

    const auto freq_text = JsonString(*repeat, "freq_type");
    parsed.freq_type = freq_text.has_value() ? ParseFrequency(*freq_text) : std::nullopt;
    if (freq_text.has_value() && !parsed.freq_type.has_value()) {
        parsed.error = "repeat.freq_type 必须是 daily、weekly、monthly 或 yearly";
        return parsed;
    }

    const auto start_time_text = JsonString(*repeat, "start_time");
    parsed.start_time = start_time_text.has_value()
                            ? schedule_tool_output::ParseLocalTime(*start_time_text)
                            : std::nullopt;
    if (start_time_text.has_value() && !parsed.start_time.has_value()) {
        parsed.error = "repeat.start_time 格式必须是 HH:mm:ss";
        return parsed;
    }

    const auto end_time_text = JsonString(*repeat, "end_time");
    parsed.end_time =
        end_time_text.has_value() ? schedule_tool_output::ParseLocalTime(*end_time_text) : std::nullopt;
    if (end_time_text.has_value() && !parsed.end_time.has_value()) {
        parsed.error = "repeat.end_time 格式必须是 HH:mm:ss";
        return parsed;
    }

    const auto start_date_text = JsonString(*repeat, "start_date");
    parsed.start_date =
        start_date_text.has_value() ? schedule_tool_output::ParseLocalDate(*start_date_text) : std::nullopt;
    if (start_date_text.has_value() && !parsed.start_date.has_value()) {
        parsed.error = "repeat.start_date 格式必须是 YYYY-MM-DD";
        return parsed;
    }

    const auto end_date_text = JsonString(*repeat, "end_date");
    parsed.end_date =
        end_date_text.has_value() ? schedule_tool_output::ParseLocalDate(*end_date_text) : std::nullopt;
    if (end_date_text.has_value() && !parsed.end_date.has_value()) {
        parsed.error = "repeat.end_date 格式必须是 YYYY-MM-DD";
        return parsed;
    }

    const auto monthly_mode_text = JsonString(*repeat, "monthly_mode");
    parsed.monthly_mode = monthly_mode_text.has_value() ? ParseMonthlyMode(*monthly_mode_text) : std::nullopt;
    if (monthly_mode_text.has_value() && !parsed.monthly_mode.has_value()) {
        parsed.error = "repeat.monthly_mode 必须是 specific_day 或 last_day";
        return parsed;
    }

    const auto interval = JsonInteger(*repeat, "interval_val");
    parsed.interval_val = interval.has_value() ? std::optional<int32_t>{static_cast<int32_t>(*interval)} : std::nullopt;

    const auto weekdays = JsonInteger(*repeat, "weekdays_mask");
    parsed.weekdays_mask =
        weekdays.has_value() ? std::optional<uint8_t>{static_cast<uint8_t>(*weekdays)} : std::nullopt;
    const auto day = JsonInteger(*repeat, "day_of_month");
    parsed.day_of_month = day.has_value() ? std::optional<uint8_t>{static_cast<uint8_t>(*day)} : std::nullopt;
    const auto month = JsonInteger(*repeat, "month_of_year");
    parsed.month_of_year = month.has_value() ? std::optional<uint8_t>{static_cast<uint8_t>(*month)} : std::nullopt;
    const auto count = JsonInteger(*repeat, "occurrence_count");
    parsed.occurrence_count = count.has_value() ? std::optional<int32_t>{static_cast<int32_t>(*count)} : std::nullopt;

    if (require_anchor && (!parsed.freq_type.has_value() || !parsed.start_time.has_value() ||
                           !parsed.start_date.has_value())) {
        parsed.error = "repeat 必须包含 freq_type、start_date 和 start_time";
    }
    return parsed;
}

schedule::CreateScheduleRuleCommand CreateRuleCommand(const PropertyList& properties, const ParsedRepeat& repeat) {
    schedule::CreateScheduleRuleCommand command;
    command.event = properties.value<std::string>("event").value_or("");
    command.location = properties.value<std::string>("location");
    command.notes = properties.value<std::string>("notes");
    command.freq_type = repeat.freq_type.value_or(schedule::Frequency::kDaily);
    command.interval_val = repeat.interval_val.value_or(1);
    command.weekdays_mask = repeat.weekdays_mask;
    command.day_of_month = repeat.day_of_month;
    command.month_of_year = repeat.month_of_year;
    command.monthly_mode = repeat.monthly_mode;
    command.start_time = repeat.start_time.value_or(schedule::LocalTime{});
    command.start_date = repeat.start_date;
    command.end_time = repeat.end_time;
    command.end_date = repeat.end_date;
    command.occurrence_count = repeat.occurrence_count;
    command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
    return command;
}

schedule::UpdateScheduleRuleCommand UpdateRuleCommand(const PropertyList& properties, const ParsedRepeat& repeat) {
    schedule::UpdateScheduleRuleCommand command;
    command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
    command.event = properties.value<std::string>("event");
    if (properties.value<std::string>("location").has_value()) {
        command.location = *properties.value<std::string>("location");
    }
    if (properties.value<std::string>("notes").has_value()) {
        command.notes = *properties.value<std::string>("notes");
    }
    if (repeat.freq_type.has_value()) command.freq_type = repeat.freq_type;
    if (repeat.interval_val.has_value()) command.interval_val = repeat.interval_val;
    if (repeat.weekdays_mask.has_value()) command.weekdays_mask = repeat.weekdays_mask;
    if (repeat.day_of_month.has_value()) command.day_of_month = repeat.day_of_month;
    if (repeat.month_of_year.has_value()) command.month_of_year = repeat.month_of_year;
    if (repeat.monthly_mode.has_value()) command.monthly_mode = repeat.monthly_mode;
    if (repeat.start_time.has_value()) command.start_time = repeat.start_time;
    if (repeat.end_time.has_value()) command.end_time = repeat.end_time;
    if (repeat.start_date.has_value()) command.start_date = repeat.start_date;
    if (repeat.end_date.has_value()) command.end_date = repeat.end_date;
    if (repeat.occurrence_count.has_value()) command.occurrence_count = repeat.occurrence_count;
    command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
    return command;
}

PropertyList RepeatProperties() {
    return PropertyList({
        Property("freq_type", PropertyType::kString)
            .with_description("周期频率，取值为 daily、weekly、monthly、yearly"),
        Property("interval_val", PropertyType::kInteger, int64_t{1})
            .with_description("周期间隔，例如 1 表示每天、每周、每月或每年一次"),
        Property("start_date", PropertyType::kString).with_description("周期规则开始日期，格式 YYYY-MM-DD"),
        Property("start_time", PropertyType::kString).with_description("周期日程每日开始时间，格式 HH:mm:ss"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("周期日程每日结束时间，格式 HH:mm:ss"),
        Property::Optional("end_date", PropertyType::kString).with_description("周期规则结束日期，格式 YYYY-MM-DD"),
        Property::Optional("occurrence_count", PropertyType::kInteger).with_description("周期规则最多发生的次数"),
        Property::Optional("weekdays_mask", PropertyType::kInteger).with_description("每周重复的星期掩码，weekly 模式使用"),
        Property::Optional("day_of_month", PropertyType::kInteger).with_description("每月重复的日期，monthly 模式使用"),
        Property::Optional("month_of_year", PropertyType::kInteger).with_description("每年重复的月份，yearly 模式使用"),
        Property::Optional("monthly_mode", PropertyType::kString)
            .with_description("月重复模式，取值为 specific_day 或 last_day"),
    });
}

PropertyList CreateProperties() {
    return PropertyList({
        Property("event", PropertyType::kString).with_description("日程标题或事件内容"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("一次性日程开始时间，格式 YYYY-MM-DD HH:mm:ss。不传表示无明确开始时间"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("一次性日程结束时间，格式 YYYY-MM-DD HH:mm:ss。不传表示无明确结束时间"),
        Property::Optional("location", PropertyType::kString).with_description("日程地点"),
        Property::Optional("notes", PropertyType::kString).with_description("日程备注"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false})
            .with_description("是否忽略时间冲突；为 true 时直接创建并返回创建后的日程"),
        Property::OptionalObject("repeat", RepeatProperties())
            .with_description("周期规则。不传时创建一次性日程，传入时创建周期日程并生成未来实例"),
    });
}

PropertyList QueryProperties() {
    return PropertyList({
        Property::Optional("keyword", PropertyType::kString).with_description("按日程标题或备注模糊搜索"),
        Property("status", PropertyType::kString, std::string("active"))
            .with_description("日程状态筛选，取值为 all、active、cancelled、completed"),
        Property::Optional("start_date", PropertyType::kString).with_description("查询开始日期，格式 YYYY-MM-DD"),
        Property::Optional("end_date", PropertyType::kString).with_description("查询结束日期，格式 YYYY-MM-DD"),
    });
}

PropertyList UpdateProperties() {
    return PropertyList({
        Property::Optional("schedule_id", PropertyType::kInteger)
            .with_description("更新或取消已物化日程时使用的日程 ID，由 schedule.query 返回"),
        Property::Optional("rule_id", PropertyType::kInteger)
            .with_description("更新未来周期实例或整条周期规则时使用的规则 ID"),
        Property::Optional("original_start_time", PropertyType::kString)
            .with_description("未来周期实例的原始发生时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("event", PropertyType::kString).with_description("新的日程标题"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("新的开始时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("新的结束时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("location", PropertyType::kString).with_description("新的地点"),
        Property::Optional("notes", PropertyType::kString).with_description("新的备注"),
        Property::Optional("status", PropertyType::kString)
            .with_description("更新日程状态；跳过某次周期日程时传 cancelled，恢复时传 active"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false}).with_description("是否忽略时间冲突"),
        Property::OptionalObject("repeat", RepeatProperties()).with_description("更新周期规则时使用的新周期配置"),
    });
}

PropertyList DeleteProperties() {
    return PropertyList({
        Property::Optional("schedule_id", PropertyType::kInteger).with_description("要删除或取消的单次日程 ID"),
        Property::Optional("rule_id", PropertyType::kInteger).with_description("要删除或取消的周期规则 ID"),
        Property::Optional("original_start_time", PropertyType::kString)
            .with_description("删除未来周期单次时使用的原始发生时间，格式 YYYY-MM-DD HH:mm:ss"),
    });
}

std::string FormatDateStart(const schedule::LocalDate& date) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d 00:00:00", date.year, date.month, date.day);
    return buffer;
}

std::string FormatDateEnd(const schedule::LocalDate& date) {
    const int64_t days = schedule::DaysFromCivil(date.year, date.month, date.day) + 1;
    schedule::LocalDate next;
    schedule::CivilFromDays(days, next.year, next.month, next.day);
    return FormatDateStart(next);
}

std::optional<DateTime> ParseDateStart(const PropertyList& properties) {
    const auto value = properties.value<std::string>("start_date");
    if (!value.has_value()) return std::nullopt;
    const auto date = schedule_tool_output::ParseLocalDate(*value);
    if (!date.has_value()) return std::nullopt;
    return schedule_tool_output::ParseDateTime(FormatDateStart(*date));
}

std::optional<DateTime> ParseDateEnd(const PropertyList& properties) {
    const auto value = properties.value<std::string>("end_date");
    if (!value.has_value()) return std::nullopt;
    const auto date = schedule_tool_output::ParseLocalDate(*value);
    if (!date.has_value()) return std::nullopt;
    return schedule_tool_output::ParseDateTime(FormatDateEnd(*date));
}

bool WithinRange(const std::optional<DateTime>& start, const std::optional<DateTime>& end, DateTime value) {
    if (start.has_value() && value < *start) return false;
    if (end.has_value() && value >= *end) return false;
    return true;
}

}  // namespace

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService* rule_service) {
    // schedule.create 根据是否传入 repeat 拆成两条业务路径：
    // 一次性日程走 ScheduleService，周期日程走 ScheduleRuleService。
    Status status = server.add_tool(
        "schedule.create", "创建一次性日程或周期日程。",
        CreateProperties(), [&service, rule_service](const PropertyList& properties) {
            const auto repeat = properties.value<JsonValue>("repeat");
            const ParsedRepeat parsed_repeat = ParseRepeat(repeat, true);
            if (!parsed_repeat.ok()) return FailureOutput(parsed_repeat.error);

            if (repeat.has_value()) {
                // 有 repeat 时创建周期规则，并把服务端物化的首条实例作为 schedule 一并返回。
                if (rule_service == nullptr) {
                    return FailureOutput("当前运行时未启用周期日程能力");
                }
                const auto result = rule_service->create_schedule_rule(CreateRuleCommand(properties, parsed_repeat));
                if (!result.status.ok()) {
                    if (result.status.code == ErrorCode::kConflict) {
                        return ConflictOutput(result.status.message,
                                              schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                    }
                    return FailureOutput(result.status.message);
                }

                ToolOutputObject fields = {
                    MakeToolOutput("status", ToolOutputValue::String("success")),
                    MakeToolOutput("message", ToolOutputValue::String("created success")),
                    MakeToolOutput("schedule", ToolOutputValue::Null()),
                    MakeToolOutput("rule", result.rule.has_value()
                                               ? schedule_tool_output::RuleOutput(*result.rule)
                                               : ToolOutputValue::Null()),
                    MakeToolOutput("conflicts",
                                   ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
                };
                if (!result.schedules.empty() && result.rule.has_value()) {
                    fields[2] = MakeToolOutput(
                        "schedule", schedule_tool_output::ScheduleOutput(result.schedules.front(), &*result.rule));
                }
                return Output(std::move(fields));
            }

            // 没有 repeat 时创建一次性日程；时间字符串在这里统一转为领域 DateTime。
            schedule::CreateScheduleCommand command;
            command.event = properties.value<std::string>("event").value_or("");
            command.start_time = properties.value<std::string>("start_time").has_value()
                                     ? schedule_tool_output::ParseDateTime(*properties.value<std::string>("start_time"))
                                     : std::nullopt;
            command.end_time = properties.value<std::string>("end_time").has_value()
                                   ? schedule_tool_output::ParseDateTime(*properties.value<std::string>("end_time"))
                                   : std::nullopt;
            if (properties.value<std::string>("start_time").has_value() && !command.start_time.has_value()) {
                return FailureOutput("start_time 格式必须是 YYYY-MM-DD HH:mm:ss");
            }
            if (properties.value<std::string>("end_time").has_value() && !command.end_time.has_value()) {
                return FailureOutput("end_time 格式必须是 YYYY-MM-DD HH:mm:ss");
            }
            command.location = properties.value<std::string>("location");
            command.notes = properties.value<std::string>("notes");
            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);

            const auto result = service.create_schedule(command);
            if (!result.result.ok()) {
                if (result.result.status.code == ErrorCode::kConflict) {
                    return ConflictOutput(result.result.status.message,
                                          schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                }
                return FailureOutput(result.result.status.message);
            }
            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String("created success")),
                MakeToolOutput("schedule", result.result.value.has_value()
                                               ? schedule_tool_output::ScheduleOutput(*result.result.value)
                                               : ToolOutputValue::Null()),
                MakeToolOutput("conflicts",
                               ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
            });
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule.query", "按自然语言友好的条件查询当前相关日程。",
        QueryProperties(), [&service, rule_service](const PropertyList& properties) {
            // query 是只读编排：先查已物化日程，再补充未来 occurrence 和周期例外，不写 schedule 表。
            const auto start = ParseDateStart(properties);
            const auto end = ParseDateEnd(properties);
            if (properties.value<std::string>("start_date").has_value() && !start.has_value()) {
                return FailureOutput("start_date 格式必须是 YYYY-MM-DD");
            }
            if (properties.value<std::string>("end_date").has_value() && !end.has_value()) {
                return FailureOutput("end_date 格式必须是 YYYY-MM-DD");
            }
            if (start.has_value() && end.has_value() && *start > *end) {
                return FailureOutput("start_date 不能晚于 end_date");
            }

            // 已物化日程仍走 ScheduleService，保证一次性日程和已生成周期实例统一从 schedule 表返回。
            schedule::QueryScheduleCommand command;
            command.keyword = properties.value<std::string>("keyword");
            command.start_from = start;
            command.start_to = end;
            command.status = ParseStatus(properties.value<std::string>("status").value_or("active"));
            command.limit = 50;
            command.offset = 0;
            const auto result = service.query_schedule(command);
            if (!result.result.ok()) return FailureOutput(result.result.status.message);

            ToolOutputArray schedules = schedule_tool_output::ScheduleArrayOutput(result.result.value);
            ToolOutputArray future_occurrences;
            ToolOutputArray exceptions;
            // 周期部分不物化，只把规则、未来 occurrence、exception 转成模型可读的结果。
            std::unordered_map<int64_t, ScheduleRule> rule_by_id;
            if (rule_service != nullptr) {
                schedule::QueryScheduleRulesCommand rule_command;
                rule_command.keyword = properties.value<std::string>("keyword");
                rule_command.status = command.status;
                rule_command.limit = 50;
                rule_command.offset = 0;
                const auto rules = rule_service->query_schedule_rules(rule_command);
                if (!rules.status.ok()) return FailureOutput(rules.status.message);

                for (const auto& view : rules.rules) {
                    rule_by_id.emplace(view.rule.id, view.rule);
                    exceptions.reserve(exceptions.size() + view.exceptions.size());
                    for (const auto& exception : view.exceptions) {
                        if (WithinRange(start, end, exception.original_start_time)) {
                            exceptions.emplace_back(MakeToolOutput(schedule_tool_output::ExceptionOutput(exception)));
                        }
                    }
                    future_occurrences.reserve(future_occurrences.size() + view.upcoming_occurrences.size());
                    for (const auto& occurrence : view.upcoming_occurrences) {
                        if (WithinRange(start, end, occurrence)) {
                            future_occurrences.emplace_back(
                                MakeToolOutput(schedule_tool_output::FutureOccurrenceOutput(view.rule, occurrence)));
                        }
                    }
                }
            }

            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String("query success")),
                MakeToolOutput("schedules", ToolOutputValue::Array(std::move(schedules))),
                MakeToolOutput("future_occurrences", ToolOutputValue::Array(std::move(future_occurrences))),
                MakeToolOutput("exceptions", ToolOutputValue::Array(std::move(exceptions))),
            });
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule.update", "更新日程、更新周期规则、取消或跳过某次日程。",
        UpdateProperties(), [&service, rule_service](const PropertyList& properties) {
            // update 根据定位参数识别目标：schedule_id 改实例，rule_id 改规则，rule_id + original_start_time 改未来单次。
            const bool has_schedule_id = properties.value<int64_t>("schedule_id").has_value();
            const bool has_rule_id = properties.value<int64_t>("rule_id").has_value();
            const bool has_original_start_time = properties.value<std::string>("original_start_time").has_value();
            const auto repeat = properties.value<JsonValue>("repeat");

            if (has_schedule_id && has_rule_id) {
                return FailureOutput("schedule_id 和 rule_id 不能同时使用");
            }
            if (has_original_start_time && !has_rule_id) {
                return FailureOutput("original_start_time 必须和 rule_id 一起使用");
            }

            if (has_schedule_id) {
                // schedule_id 命中已物化实例；status=cancelled 走取消，否则走一次性日程更新。
                const auto status_text = properties.value<std::string>("status");
                if (status_text.has_value() && *status_text == "cancelled") {
                    schedule::CancelScheduleCommand command;
                    command.schedule_id = properties.value<int64_t>("schedule_id").value_or(0);
                    const auto result = service.cancel_schedule(command);
                    if (!result.result.ok()) return FailureOutput(result.result.status.message);
                    return Output({
                        MakeToolOutput("status", ToolOutputValue::String("success")),
                        MakeToolOutput("message", ToolOutputValue::String("deleted success")),
                        MakeToolOutput("schedule", ToolOutputValue::Null()),
                        MakeToolOutput("rule", ToolOutputValue::Null()),
                        MakeToolOutput("exception", ToolOutputValue::Null()),
                        MakeToolOutput("conflicts", ToolOutputValue::Array(ToolOutputArray{})),
                    });
                }

                schedule::UpdateScheduleCommand command;
                command.schedule_id = *properties.value<int64_t>("schedule_id");
                if (properties.value<std::string>("event").has_value()) command.event = *properties.value<std::string>("event");
                if (properties.value<std::string>("start_time").has_value()) {
                    const auto parsed = schedule_tool_output::ParseDateTime(*properties.value<std::string>("start_time"));
                    if (!parsed.has_value()) return FailureOutput("start_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                    command.start_time = parsed;
                }
                if (properties.value<std::string>("end_time").has_value()) {
                    const auto parsed = schedule_tool_output::ParseDateTime(*properties.value<std::string>("end_time"));
                    if (!parsed.has_value()) return FailureOutput("end_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                    command.end_time = parsed;
                }
                if (properties.value<std::string>("location").has_value()) command.location = *properties.value<std::string>("location");
                if (properties.value<std::string>("notes").has_value()) command.notes = *properties.value<std::string>("notes");
                command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);

                const auto result = service.update_schedule(command);
                if (!result.result.ok()) {
                    if (result.result.status.code == ErrorCode::kConflict) {
                        return ConflictOutput(result.result.status.message,
                                              schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                    }
                    return FailureOutput(result.result.status.message);
                }
                return Output({
                    MakeToolOutput("status", ToolOutputValue::String("success")),
                    MakeToolOutput("message", ToolOutputValue::String("updated success")),
                    MakeToolOutput("schedule", result.result.value.has_value()
                                                   ? schedule_tool_output::ScheduleOutput(*result.result.value)
                                                   : ToolOutputValue::Null()),
                    MakeToolOutput("rule", ToolOutputValue::Null()),
                    MakeToolOutput("exception", ToolOutputValue::Null()),
                    MakeToolOutput("conflicts",
                                   ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
                });
            }

            if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力");

            if (has_original_start_time) {
                // 未来周期单次没有 schedule_id，通过 rule_id + original_start_time 定位。
                const auto original = schedule_tool_output::ParseDateTime(
                    properties.value<std::string>("original_start_time").value_or(""));
                if (!original.has_value()) {
                    return FailureOutput("original_start_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                }
                const auto status_text = properties.value<std::string>("status");
                if (status_text.has_value() && *status_text == "cancelled") {
                    // 跳过未来单次：在 schedule_rule_exception 中记录 skip，后续生成时不再物化这次。
                    schedule::SkipScheduleOccurrenceCommand command;
                    command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
                    command.original_start_time = *original;
                    const auto result = rule_service->skip_schedule_occurrence(command);
                    if (!result.status.ok()) return FailureOutput(result.status.message);
                    return Output({
                        MakeToolOutput("status", ToolOutputValue::String("success")),
                        MakeToolOutput("message", ToolOutputValue::String("updated success")),
                        MakeToolOutput("schedule", ToolOutputValue::Null()),
                        MakeToolOutput("rule", ToolOutputValue::Null()),
                        MakeToolOutput("exception", result.exception.has_value()
                                                       ? schedule_tool_output::ExceptionOutput(*result.exception)
                                                       : ToolOutputValue::Null()),
                        MakeToolOutput("conflicts", ToolOutputValue::Array(ToolOutputArray{})),
                    });
                }

                // 修改未来单次：先落到 schedule_rule_exception，后续物化该次时使用覆盖字段。
                schedule::UpdateScheduleOccurrenceCommand command;
                command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
                command.original_start_time = *original;
                if (properties.value<std::string>("event").has_value()) command.event = std::optional<std::string>{*properties.value<std::string>("event")};
                if (properties.value<std::string>("start_time").has_value()) {
                    const auto parsed = schedule_tool_output::ParseDateTime(*properties.value<std::string>("start_time"));
                    if (!parsed.has_value()) return FailureOutput("start_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                    command.start_time = std::optional<DateTime>{*parsed};
                }
                if (properties.value<std::string>("end_time").has_value()) {
                    const auto parsed = schedule_tool_output::ParseDateTime(*properties.value<std::string>("end_time"));
                    if (!parsed.has_value()) return FailureOutput("end_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                    command.end_time = std::optional<DateTime>{*parsed};
                }
                if (properties.value<std::string>("location").has_value()) command.location = std::optional<std::string>{*properties.value<std::string>("location")};
                if (properties.value<std::string>("notes").has_value()) command.notes = std::optional<std::string>{*properties.value<std::string>("notes")};
                command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
                const auto result = rule_service->update_schedule_occurrence(command);
                if (!result.status.ok()) return FailureOutput(result.status.message);
                return Output({
                    MakeToolOutput("status", ToolOutputValue::String("success")),
                    MakeToolOutput("message", ToolOutputValue::String("updated success")),
                    MakeToolOutput("schedule", ToolOutputValue::Null()),
                    MakeToolOutput("rule", ToolOutputValue::Null()),
                    MakeToolOutput("exception", result.exception.has_value()
                                                   ? schedule_tool_output::ExceptionOutput(*result.exception)
                                                   : ToolOutputValue::Null()),
                    MakeToolOutput("conflicts", ToolOutputValue::Array(ToolOutputArray{})),
                });
            }

            if (!has_rule_id) return FailureOutput("请提供 schedule_id、rule_id 或 rule_id + original_start_time");
            // 只有 rule_id 时按整条周期规则更新；repeat 提供新规则字段，未传字段由 service 保持原值。
            const ParsedRepeat parsed_repeat = ParseRepeat(repeat, false);
            if (!parsed_repeat.ok()) return FailureOutput(parsed_repeat.error);
            const auto result = rule_service->update_schedule_rule(UpdateRuleCommand(properties, parsed_repeat));
            if (!result.status.ok()) {
                if (result.status.code == ErrorCode::kConflict) {
                    return ConflictOutput(result.status.message,
                                          schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                }
                return FailureOutput(result.status.message);
            }
            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String("updated success")),
                MakeToolOutput("schedule", ToolOutputValue::Null()),
                MakeToolOutput("rule", result.rule.has_value() ? schedule_tool_output::RuleOutput(*result.rule)
                                                               : ToolOutputValue::Null()),
                MakeToolOutput("exception", ToolOutputValue::Null()),
                MakeToolOutput("conflicts",
                               ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
            });
        });
    if (!status.ok()) return status;

    return server.add_tool(
        "schedule.delete", "删除单次日程、未来周期单次或整条周期规则。",
        DeleteProperties(), [&service, rule_service](const PropertyList& properties) {
            // delete 根据定位参数拆三条路径：schedule_id 删实例，rule_id 删规则，rule_id + original_start_time 跳过未来单次。
            const bool has_schedule_id = properties.value<int64_t>("schedule_id").has_value();
            const bool has_rule_id = properties.value<int64_t>("rule_id").has_value();
            const bool has_original_start_time = properties.value<std::string>("original_start_time").has_value();
            if (!has_schedule_id && !has_rule_id) return FailureOutput("请提供 schedule_id 或 rule_id");
            if (has_schedule_id && has_rule_id) return FailureOutput("schedule_id 和 rule_id 不能同时使用");

            if (has_schedule_id) {
                // 删除实例前先读取快照，取消成功后把快照状态改为 cancelled 返回给模型。
                const schedule::ScheduleId schedule_id = properties.value<int64_t>("schedule_id").value_or(0);
                schedule::QueryScheduleCommand query;
                query.schedule_id = schedule_id;
                query.status = schedule::ScheduleStatusFilter::kAll;
                query.limit = 1;
                query.offset = 0;
                const auto loaded = service.query_schedule(query);
                if (!loaded.result.ok() || loaded.result.value.empty()) return FailureOutput("日程不存在");
                const auto result = service.cancel_schedule({.schedule_id = schedule_id});
                if (!result.result.ok()) return FailureOutput(result.result.status.message);
                schedule::Schedule deleted = loaded.result.value.front();
                deleted.status = schedule::ScheduleStatus::kCancelled;
                return Output({
                    MakeToolOutput("status", ToolOutputValue::String("success")),
                    MakeToolOutput("message", ToolOutputValue::String("deleted success")),
                    MakeToolOutput("schedule", schedule_tool_output::ScheduleOutput(deleted)),
                    MakeToolOutput("rule", ToolOutputValue::Null()),
                    MakeToolOutput("exception", ToolOutputValue::Null()),
                });
            }

            if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力");
            if (has_original_start_time) {
                // 删除未来周期单次等价于创建 skip exception，不落库为 schedule。
                const auto original = schedule_tool_output::ParseDateTime(
                    properties.value<std::string>("original_start_time").value_or(""));
                if (!original.has_value()) {
                    return FailureOutput("original_start_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                }
                schedule::SkipScheduleOccurrenceCommand command;
                command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
                command.original_start_time = *original;
                const auto result = rule_service->skip_schedule_occurrence(command);
                if (!result.status.ok()) return FailureOutput(result.status.message);
                return Output({
                    MakeToolOutput("status", ToolOutputValue::String("success")),
                    MakeToolOutput("message", ToolOutputValue::String("deleted success")),
                    MakeToolOutput("schedule", ToolOutputValue::Null()),
                    MakeToolOutput("rule", ToolOutputValue::Null()),
                    MakeToolOutput("exception", result.exception.has_value()
                                                   ? schedule_tool_output::ExceptionOutput(*result.exception)
                                                   : ToolOutputValue::Null()),
                });
            }

            // 仅 rule_id 时取消整条周期规则及其未来实例。
            schedule::CancelScheduleRuleCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            const auto result = rule_service->cancel_schedule_rule(command);
            if (!result.status.ok()) return FailureOutput(result.status.message);
            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String("deleted success")),
                MakeToolOutput("schedule", ToolOutputValue::Null()),
                MakeToolOutput("rule", result.rule.has_value() ? schedule_tool_output::RuleOutput(*result.rule)
                                                               : ToolOutputValue::Null()),
                MakeToolOutput("exception", ToolOutputValue::Null()),
            });
        });
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service) {
    return RegisterScheduleMcpTools(server, service, nullptr);
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService& rule_service) {
    return RegisterScheduleMcpTools(server, service, &rule_service);
}

}  // namespace voicelife::mcp
