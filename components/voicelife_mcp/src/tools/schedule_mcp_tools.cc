#include "voicelife/mcp/schedule_mcp_tools.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "schedule_mcp_tools_input.h"
#include "schedule_tool_output.h"
#include "voicelife/contracts/im/reminder_action_status_report.h"
#include "voicelife/contracts/im/schedule_query_result.h"
#include "voicelife/im/im_reporting_channel.h"
#include "voicelife/im/im_runtime.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_operation_service.h"
#include "voicelife/schedule/schedule_reminder_service.h"
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
using voicelife::mcp::schedule_tool_input::CreateProperties;
using voicelife::mcp::schedule_tool_input::CreateRuleCommand;
using voicelife::mcp::schedule_tool_input::CreateRuleProperties;
using voicelife::mcp::schedule_tool_input::DeleteProperties;
using voicelife::mcp::schedule_tool_input::DeleteRuleProperties;
using voicelife::mcp::schedule_tool_input::OperationQueryProperties;
using voicelife::mcp::schedule_tool_input::SkipOccurrenceProperties;
using voicelife::mcp::schedule_tool_input::ParsedRepeat;
using voicelife::mcp::schedule_tool_input::ParseRepeat;
using voicelife::mcp::schedule_tool_input::ParseRuleProperties;
using voicelife::mcp::schedule_tool_input::QueryProperties;
using voicelife::mcp::schedule_tool_input::UpdateOccurrenceProperties;
using voicelife::mcp::schedule_tool_input::UpdateProperties;
using voicelife::mcp::schedule_tool_input::UpdateRuleCommand;
using voicelife::mcp::schedule_tool_input::UpdateRuleProperties;

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

/** @brief 返回当前秒级系统时间。 @return 当前日程时间。 */
DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

std::string NowIso() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::optional<JsonValue> OutputJson(const ToolOutputValue& output) {
    JsonValue value;
    JsonParseOptions options;
    options.max_bytes = 128 * 1024;
    options.max_nodes = 4096;
    options.max_array_items = 128;
    options.max_allocator_bytes = 512 * 1024;
    if (!ParseJson(SerializeToolOutputValue(output), value, options).ok()) return std::nullopt;
    return value;
}

/** @brief 将实体类型字符串转为枚举；非法值返回空。 @param value 输入字符串。 @return 对应枚举。 */
std::optional<schedule::OperationEntityType> ParseEntityType(const std::string& value) {
    if (value == "schedule") return schedule::OperationEntityType::kSchedule;
    if (value == "rule") return schedule::OperationEntityType::kRule;
    if (value == "exception") return schedule::OperationEntityType::kException;
    return std::nullopt;
}

/** @brief 将操作类型字符串转为枚举；非法值返回空。 @param value 输入字符串。 @return 对应枚举。 */
std::optional<schedule::ScheduleOperationType> ParseOperationType(const std::string& value) {
    if (value == "create") return schedule::ScheduleOperationType::kCreate;
    if (value == "update") return schedule::ScheduleOperationType::kUpdate;
    if (value == "delete") return schedule::ScheduleOperationType::kDelete;
    return std::nullopt;
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

std::optional<ToolResult> SynchronizeReminder(schedule::ScheduleReminderService* reminder_service,
                                              schedule::ScheduleId schedule_id) {
    if (reminder_service == nullptr) return std::nullopt;
    const Status status = reminder_service->SynchronizeSchedule(schedule_id);
    if (status.ok()) return std::nullopt;
    return FailureOutput("日程已保存，但提醒同步失败：" + status.message);
}

std::optional<ToolResult> CancelReminder(schedule::ScheduleReminderService* reminder_service,
                                         schedule::ScheduleId schedule_id) {
    if (reminder_service == nullptr) return std::nullopt;
    const Status status = reminder_service->CancelScheduleReminder(schedule_id);
    if (status.ok()) return std::nullopt;
    return FailureOutput("日程已取消，但提醒取消失败：" + status.message);
}

std::optional<ToolResult> VerifyCancellationTarget(const schedule::Schedule& schedule, const PropertyList& properties) {
    const auto expected_event = properties.value<std::string>("expected_event");
    const auto expected_start_time = properties.value<std::string>("expected_start_time");
    if (!expected_event.has_value() || !expected_start_time.has_value()) {
        return FailureOutput("请先通过 schedule.query 确认目标，并回传 event 和 start_time");
    }
    if (!schedule.start_time.has_value() || schedule.event != *expected_event ||
        schedule_tool_output::FormatDateTime(*schedule.start_time) != *expected_start_time) {
        return FailureOutput("日程目标与确认内容不匹配，未执行取消");
    }
    return std::nullopt;
}

std::optional<ToolResult> SuspendRuleReminders(schedule::ScheduleReminderService* reminder_service,
                                               schedule::ScheduleRuleId rule_id) {
    if (reminder_service == nullptr) return std::nullopt;
    const Status status = reminder_service->SuspendRuleReminders(rule_id);
    if (status.ok()) return std::nullopt;
    return FailureOutput("周期规则已修改，但旧提醒撤销失败：" + status.message);
}

std::optional<ToolResult> SynchronizeRule(schedule::ScheduleReminderService* reminder_service,
                                          schedule::ScheduleRuleId rule_id) {
    if (reminder_service == nullptr) return std::nullopt;
    const Status status = reminder_service->SynchronizeRule(rule_id);
    if (status.ok()) return std::nullopt;
    return FailureOutput("周期规则已修改，但提醒同步失败：" + status.message);
}

bool WithinRange(const std::optional<DateTime>& start, const std::optional<DateTime>& end, DateTime value) {
    if (start.has_value() && value < *start) return false;
    if (end.has_value() && value >= *end) return false;
    return true;
}

std::string IsoFromDateTime(DateTime value) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
    const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

const char* ActionName(schedule::ScheduleReminderActionKind action) {
    return action == schedule::ScheduleReminderActionKind::kSnooze ? "snooze" : "acknowledge";
}

bool ReportVoiceActionResults(const std::vector<schedule::ReminderActionResult>& results,
                              ScheduleQueryReportingContext reporting_context) {
    auto* runtime = reporting_context.runtime;
    auto* channel = runtime == nullptr ? nullptr : runtime->reporting_channel();
    // 本地事实已经落库；IM 未 ready 时由 Runtime worker 补报，不能把“未发送”伪报为成功。
    if (channel == nullptr || runtime == nullptr || runtime->device_id().empty()) return false;
    bool all_submitted = true;
    for (const auto& result : results) {
        contracts::im::ReminderActionStatusReport report;
        report.schemaVersion = "1";
        report.eventId = "voice-action:" + result.operation_id;
        report.correlationId = result.operation_id;
        report.deviceId = runtime->device_id();
        report.reminderTriggerId = result.reminder_trigger_id;
        report.operationId = result.operation_id;
        report.action = ActionName(result.action);
        report.status = "succeeded";
        report.occurredAt = IsoFromDateTime(result.occurred_at);
        if (result.next_trigger_at.has_value()) report.nextTriggerAt = IsoFromDateTime(*result.next_trigger_at);
        report.source = "voice";
        const auto submitted = channel->SubmitReminderActionStatusReport(report);
        if (submitted.status != voicelife::im::ReportStatus::kSubmitted) all_submitted = false;
    }
    return all_submitted;
}

}  // namespace

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService* rule_service,
                                schedule::ScheduleOperationService* operation_service,
                                schedule::ScheduleReminderService* reminder_service,
                                ScheduleQueryReportingContext reporting_context) {
    Status status = server.add_tool(
        "schedule.create",
        "创建一条一次性日程并直接写入 schedule 表。只能创建独立的一次性日程；不要传 repeat 或任何周期规则字段。",
        CreateProperties(), [&service, reminder_service](const PropertyList& properties) {
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
            if (result.result.value.has_value()) {
                const std::optional<ToolResult> reminder_status =
                    SynchronizeReminder(reminder_service, result.result.value->id);
                if (reminder_status.has_value()) return *reminder_status;
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

    if (rule_service != nullptr) {
        status = server.add_tool(
            "schedule.create_rule",
            "创建周期日程：在 schedule_rule 表创建周期规则，并物化首条 schedule 实例。周期字段必须直接作为顶层参数传入，不使用 repeat 对象。",
            CreateRuleProperties(), [rule_service, reminder_service](const PropertyList& properties) {
                if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力");
                const ParsedRepeat parsed_repeat = ParseRuleProperties(properties, true);
                if (!parsed_repeat.ok()) return FailureOutput(parsed_repeat.error);

                const auto result = rule_service->create_schedule_rule(CreateRuleCommand(properties, parsed_repeat));
                if (!result.status.ok()) {
                    if (result.status.code == ErrorCode::kConflict) {
                        return ConflictOutput(result.status.message,
                                              schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                    }
                    return FailureOutput(result.status.message);
                }
                if (result.rule.has_value()) {
                    const std::optional<ToolResult> reminder_status =
                        SynchronizeRule(reminder_service, result.rule->id);
                    if (reminder_status.has_value()) return *reminder_status;
                }
                return Output({
                    MakeToolOutput("status", ToolOutputValue::String("success")),
                    MakeToolOutput("message", ToolOutputValue::String("created success")),
                    MakeToolOutput("rule", result.rule.has_value() ? schedule_tool_output::RuleOutput(*result.rule)
                                                                   : ToolOutputValue::Null()),
                    MakeToolOutput("schedule", (!result.schedules.empty() && result.rule.has_value())
                                                   ? schedule_tool_output::ScheduleOutput(result.schedules.front(), &*result.rule)
                                                   : ToolOutputValue::Null()),
                    MakeToolOutput("conflicts",
                                   ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
                });
            });
        if (!status.ok()) return status;
    }

    status = server.add_tool_with_context(
        "schedule.query",
        "统一查询一次性日程和周期日程。返回结果按 one_time_schedules、recurring_rules、recurring_schedules、future_occurrences、exceptions 分类；schedule_id 与 rule_id 互斥。",
        QueryProperties(), [&service, rule_service, reporting_context](const ToolCall& call) {
            const PropertyList properties = QueryProperties().with_values(call.arguments);
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

            const auto schedule_id = properties.value<int64_t>("schedule_id");
            const auto rule_id = properties.value<int64_t>("rule_id");
            if (schedule_id.has_value() && rule_id.has_value()) {
                return FailureOutput("schedule_id 和 rule_id 不能同时传入；查询 schedule 使用前者，查询周期规则使用后者");
            }

            schedule::QueryScheduleCommand command;
            command.schedule_id = schedule_id;
            command.rule_id = rule_id;
            command.keyword = properties.value<std::string>("keyword");
            command.start_from = start;
            command.start_to = end;
            command.status = ParseStatus(properties.value<std::string>("status").value_or("active"));
            command.limit = 50;
            command.offset = 0;
            const auto result = service.query_schedule(command);
            if (!result.result.ok()) {
                return FailureOutput(result.result.status.message.empty() ? "查询已物化日程失败"
                                                                         : result.result.status.message);
            }

            ToolOutputArray one_time;
            ToolOutputArray recurring_schedules;
            for (const auto& item : result.result.value) {
                if (item.rule_id.has_value()) {
                    recurring_schedules.emplace_back(MakeToolOutput(schedule_tool_output::ScheduleOutput(item)));
                } else {
                    one_time.emplace_back(MakeToolOutput(schedule_tool_output::ScheduleOutput(item)));
                }
            }

            ToolOutputArray recurring_rules;
            ToolOutputArray future_occurrences;
            ToolOutputArray exceptions;
            if (rule_service != nullptr && !schedule_id.has_value()) {
                schedule::QueryScheduleRulesCommand rule_command;
                rule_command.rule_id = rule_id;
                rule_command.keyword = properties.value<std::string>("keyword");
                rule_command.status = command.status;
                rule_command.occurrence_start = start;
                rule_command.occurrence_end = end;
                rule_command.limit = 50;
                rule_command.offset = 0;
                const auto rules = rule_service->query_schedule_rules(rule_command);
                if (!rules.status.ok()) {
                    return FailureOutput(rules.status.message.empty() ? "查询周期规则失败" : rules.status.message);
                }
                for (const auto& view : rules.rules) {
                    recurring_rules.emplace_back(MakeToolOutput(schedule_tool_output::RuleOutput(view.rule)));
                    for (const auto& exception : view.exceptions) {
                        if (exceptions.size() < contracts::im::kMaxScheduleQueryItems &&
                            WithinRange(start, end, exception.original_start_time)) {
                            exceptions.emplace_back(MakeToolOutput(schedule_tool_output::ExceptionOutput(exception)));
                        }
                    }
                    for (const auto& occurrence : view.upcoming_occurrences) {
                        if (future_occurrences.size() < contracts::im::kMaxScheduleQueryItems &&
                            WithinRange(start, end, occurrence)) {
                            future_occurrences.emplace_back(
                                MakeToolOutput(schedule_tool_output::FutureOccurrenceOutput(view.rule, occurrence)));
                        }
                    }
                }
            }

            const int64_t result_count = static_cast<int64_t>(one_time.size() + recurring_schedules.size() +
                                                              future_occurrences.size());
            const std::string keyword = properties.value<std::string>("keyword").value_or("");
            const std::string prefix = keyword.empty() ? "查询到" : "根据“" + keyword + "”关键字查询到";
            const std::string message = prefix + " " + std::to_string(one_time.size()) + " 条一次性日程、" +
                                        std::to_string(recurring_rules.size()) + " 条周期规则、" +
                                        std::to_string(recurring_schedules.size()) + " 条周期实例和 " +
                                        std::to_string(future_occurrences.size()) + " 条未来 occurrence";

            const auto one_time_json = OutputJson(ToolOutputValue::Array(one_time));
            const auto future_json = OutputJson(ToolOutputValue::Array(future_occurrences));
            const auto exceptions_json = OutputJson(ToolOutputValue::Array(exceptions));
            if (!one_time_json.has_value() || !future_json.has_value() || !exceptions_json.has_value()) {
                return FailureOutput("查询结果序列化失败");
            }

            auto* reporting_channel =
                reporting_context.runtime == nullptr ? nullptr : reporting_context.runtime->reporting_channel();
            const std::string reporting_device_id =
                reporting_context.runtime == nullptr ? std::string{} : reporting_context.runtime->device_id();
            const char* report_state = "failed";
            if (reporting_channel != nullptr && !reporting_device_id.empty()) {
                contracts::im::ScheduleQueryResultIntent intent;
                intent.schemaVersion = "1";
                intent.businessEventId = "schedule-query:" + call.request_id;
                intent.correlationId = call.request_id;
                intent.userId = reporting_context.runtime->user_id();
                intent.deviceId = reporting_device_id;
                intent.keyword = properties.value<std::string>("keyword");
                intent.status = properties.value<std::string>("status").value_or("active");
                intent.startDate = properties.value<std::string>("start_date");
                intent.endDate = properties.value<std::string>("end_date");
                intent.resultCount = result_count;
                intent.schedules = *one_time_json;
                intent.futureOccurrences = *future_json;
                intent.exceptions = *exceptions_json;
                intent.queriedAt = NowIso();
                const voicelife::im::ReportResult report = reporting_channel->SubmitScheduleQueryResult(intent);
                report_state = report.status == voicelife::im::ReportStatus::kSubmitted
                                   ? "submitted"
                                   : (report.status == voicelife::im::ReportStatus::kRetryable ? "retryable_failed"
                                                                                               : "failed");
            }

            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String(message)),
                MakeToolOutput("result_count", ToolOutputValue::Integer(result_count)),
                MakeToolOutput("one_time_schedules", ToolOutputValue::Array(std::move(one_time))),
                MakeToolOutput("recurring_rules", ToolOutputValue::Array(std::move(recurring_rules))),
                MakeToolOutput("recurring_schedules", ToolOutputValue::Array(std::move(recurring_schedules))),
                MakeToolOutput("future_occurrences", ToolOutputValue::Array(std::move(future_occurrences))),
                MakeToolOutput("exceptions", ToolOutputValue::Array(std::move(exceptions))),
                MakeToolOutput("im_delivery", reporting_channel == nullptr
                                                  ? ToolOutputValue::Null()
                                                  : ToolOutputValue::String(report_state)),
            });
        });
    if (!status.ok()) return status;
    status = server.add_tool(
        "schedule.update",
        "修改一次性日程或已经物化到 schedule 表的周期实例。必须只传 schedule_id；不要传 rule_id、original_start_time 或周期规则字段。",
        UpdateProperties(), [&service, reminder_service](const PropertyList& properties) {
            const auto schedule_id = properties.value<int64_t>("schedule_id");
            if (!schedule_id.has_value()) return FailureOutput("请提供 schedule_id");

            schedule::UpdateScheduleCommand command;
            command.schedule_id = *schedule_id;
            if (properties.value<std::string>("event").has_value())
                command.event = *properties.value<std::string>("event");
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
            if (properties.value<std::string>("location").has_value())
                command.location = *properties.value<std::string>("location");
            if (properties.value<std::string>("notes").has_value())
                command.notes = *properties.value<std::string>("notes");
            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);

            const auto result = service.update_schedule(command);
            if (!result.result.ok()) {
                if (result.result.status.code == ErrorCode::kConflict) {
                    return ConflictOutput(result.result.status.message,
                                          schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                }
                return FailureOutput(result.result.status.message);
            }
            if (result.result.value.has_value()) {
                const std::optional<ToolResult> reminder_status =
                    SynchronizeReminder(reminder_service, result.result.value->id);
                if (reminder_status.has_value()) return *reminder_status;
            }
            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String("updated success")),
                MakeToolOutput("schedule", result.result.value.has_value()
                                               ? schedule_tool_output::ScheduleOutput(*result.result.value)
                                               : ToolOutputValue::Null()),
                MakeToolOutput("conflicts",
                               ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
            });
        });
    if (!status.ok()) return status;

    if (rule_service != nullptr) {
        status = server.add_tool(
            "schedule.update_occurrence",
            "修改未来周期中的某一次尚未物化 occurrence。必须传 rule_id + original_start_time；已物化时请改用 schedule.update。",
            UpdateOccurrenceProperties(), [rule_service](const PropertyList& properties) {
                if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力");
                const auto original = schedule_tool_output::ParseDateTime(
                    properties.value<std::string>("original_start_time").value_or(""));
                if (!original.has_value()) return FailureOutput("original_start_time 格式必须是 YYYY-MM-DD HH:mm:ss");

                schedule::UpdateScheduleOccurrenceCommand command;
                command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
                command.original_start_time = *original;
                if (properties.value<std::string>("event").has_value())
                    command.event = std::optional<std::string>{*properties.value<std::string>("event")};
                if (properties.value<std::string>("start_time").has_value()) {
                    const auto parsed =
                        schedule_tool_output::ParseDateTime(*properties.value<std::string>("start_time"));
                    if (!parsed.has_value()) return FailureOutput("start_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                    command.start_time = std::optional<DateTime>{*parsed};
                }
                if (properties.value<std::string>("end_time").has_value()) {
                    const auto parsed = schedule_tool_output::ParseDateTime(*properties.value<std::string>("end_time"));
                    if (!parsed.has_value()) return FailureOutput("end_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                    command.end_time = std::optional<DateTime>{*parsed};
                }
                if (properties.value<std::string>("location").has_value())
                    command.location = std::optional<std::string>{*properties.value<std::string>("location")};
                if (properties.value<std::string>("notes").has_value())
                    command.notes = std::optional<std::string>{*properties.value<std::string>("notes")};
                command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);

                const auto result = rule_service->update_schedule_occurrence(command);
                if (!result.status.ok()) return FailureOutput(result.status.message);
                return Output({
                    MakeToolOutput("status", ToolOutputValue::String("success")),
                    MakeToolOutput("message", ToolOutputValue::String("updated success")),
                    MakeToolOutput("exception", result.exception.has_value()
                                                    ? schedule_tool_output::ExceptionOutput(*result.exception)
                                                    : ToolOutputValue::Null()),
                    MakeToolOutput("conflicts", ToolOutputValue::Array(ToolOutputArray{})),
                });
            });
        if (!status.ok()) return status;
    }

    if (rule_service != nullptr) {
        status = server.add_tool(
            "schedule.update_rule",
            "修改整条周期规则并按新规则重建未来实例。必须只传 rule_id；周期字段直接作为顶层参数传入，不使用 repeat 对象。",
            UpdateRuleProperties(), [rule_service, reminder_service](const PropertyList& properties) {
                if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力");
                const ParsedRepeat parsed_repeat = ParseRuleProperties(properties, false);
                if (!parsed_repeat.ok()) return FailureOutput(parsed_repeat.error);

                const schedule::ScheduleRuleId rule_id = properties.value<int64_t>("rule_id").value_or(0);
                const std::optional<ToolResult> suspended = SuspendRuleReminders(reminder_service, rule_id);
                if (suspended.has_value()) return *suspended;
                const auto result = rule_service->update_schedule_rule(UpdateRuleCommand(properties, parsed_repeat));
                if (!result.status.ok()) {
                    (void)SynchronizeRule(reminder_service, rule_id);
                    if (result.status.code == ErrorCode::kConflict) {
                        return ConflictOutput(result.status.message,
                                              schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                    }
                    return FailureOutput(result.status.message);
                }
                const std::optional<ToolResult> reminder_status = SynchronizeRule(reminder_service, rule_id);
                if (reminder_status.has_value()) return *reminder_status;
                return Output({
                    MakeToolOutput("status", ToolOutputValue::String("success")),
                    MakeToolOutput("message", ToolOutputValue::String("updated success")),
                    MakeToolOutput("rule", result.rule.has_value() ? schedule_tool_output::RuleOutput(*result.rule)
                                                                   : ToolOutputValue::Null()),
                    MakeToolOutput("conflicts",
                                   ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
                });
            });
        if (!status.ok()) return status;
    }
    status = server.add_tool(
        "schedule.delete",
        "取消一次性日程或已经物化到 schedule 表的周期实例。必须传 schedule_id、expected_event、expected_start_time；这三个字段用于确认具体记录。不要传 rule_id 或 original_start_time。",
        DeleteProperties(), [&service, reminder_service](const PropertyList& properties) {
            const schedule::ScheduleId id = properties.value<int64_t>("schedule_id").value_or(0);
            schedule::QueryScheduleCommand query;
            query.schedule_id = id;
            query.status = schedule::ScheduleStatusFilter::kAll;
            query.limit = 1;
            query.offset = 0;
            const auto loaded = service.query_schedule(query);
            if (!loaded.result.ok() || loaded.result.value.empty()) {
                return FailureOutput("找不到 schedule_id=" + std::to_string(id) + " 对应的日程");
            }
            if (const auto check = VerifyCancellationTarget(loaded.result.value.front(), properties);
                check.has_value()) {
                return *check;
            }
            const auto result = service.cancel_schedule({.schedule_id = id});
            if (!result.result.ok()) {
                return FailureOutput(result.result.status.message.empty() ? "日程取消失败" : result.result.status.message);
            }
            if (const auto reminder = CancelReminder(reminder_service, id); reminder.has_value()) return *reminder;
            schedule::Schedule cancelled = loaded.result.value.front();
            cancelled.status = schedule::ScheduleStatus::kCancelled;
            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String("已取消 schedule_id=" + std::to_string(id) + " 的日程")),
                MakeToolOutput("schedule", schedule_tool_output::ScheduleOutput(cancelled)),
                MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{})),
            });
        });
    if (!status.ok()) return status;

    if (rule_service != nullptr) {
        status = server.add_tool(
            "schedule.delete_rule",
            "取消整条周期规则及其已物化实例，并停止后续 occurrence 生成。必须只传 rule_id；不要传 schedule_id 或 original_start_time。",
            DeleteRuleProperties(), [rule_service, reminder_service](const PropertyList& properties) {
                if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力，无法取消规则");
                const auto id = properties.value<int64_t>("rule_id").value_or(0);
                const auto result = rule_service->cancel_schedule_rule({.rule_id = id});
                if (!result.status.ok()) {
                    return FailureOutput(result.status.message.empty() ? "周期规则取消失败" : result.status.message);
                }
                if (const auto reminder = SuspendRuleReminders(reminder_service, id); reminder.has_value()) return *reminder;
                return Output({
                    MakeToolOutput("status", ToolOutputValue::String("success")),
                    MakeToolOutput("message", ToolOutputValue::String("已取消周期规则 rule_id=" + std::to_string(id) + "，后续 occurrence 将不再生成")),
                    MakeToolOutput("rule", result.rule.has_value() ? schedule_tool_output::RuleOutput(*result.rule)
                                                                   : ToolOutputValue::Null()),
                    MakeToolOutput("cancelled_schedule_count", ToolOutputValue::Integer(result.cancelled_count)),
                    MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{})),
                });
            });
        if (!status.ok()) return status;
    }

    if (rule_service != nullptr) {
        status = server.add_tool(
            "schedule.skip_occurrence",
            "跳过未来周期中的某一次尚未物化 occurrence，实际写入 schedule_rule_exception，而不是删除周期规则。必须传 rule_id + original_start_time + expected_event；不要传 schedule_id。已物化时请改用 schedule.delete。",
            SkipOccurrenceProperties(), [rule_service](const PropertyList& properties) {
                if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力，无法跳过 occurrence");
                const auto original = schedule_tool_output::ParseDateTime(
                    properties.value<std::string>("original_start_time").value_or(""));
                if (!original.has_value()) {
                    return FailureOutput("original_start_time 必须是严格的 YYYY-MM-DD HH:mm:ss 完整本地时间");
                }
                schedule::SkipScheduleOccurrenceCommand command{
                    .rule_id = properties.value<int64_t>("rule_id").value_or(0),
                    .original_start_time = *original,
                };
                const auto result = rule_service->skip_schedule_occurrence(command);
                if (!result.status.ok()) {
                    return FailureOutput(result.status.message.empty() ? "跳过未来 occurrence 失败；如果已物化请改用 schedule.delete"
                                                                     : result.status.message);
                }
                if (result.exception.has_value() && result.exception->type == schedule::ExceptionType::kSkip) {
                    return Output({
                        MakeToolOutput("status", ToolOutputValue::String("success")),
                        MakeToolOutput("message", ToolOutputValue::String("已跳过周期规则 rule_id=" + std::to_string(command.rule_id) + " 在 " + properties.value<std::string>("original_start_time").value() + " 的 occurrence")),
                        MakeToolOutput("exception", schedule_tool_output::ExceptionOutput(*result.exception)),
                        MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{})),
                    });
                }
                return FailureOutput("跳过 occurrence 未返回有效 exception");
            });
        if (!status.ok()) return status;
    }

    // 只有装配操作记录服务时才暴露查询工具；基础运行时保持原有四个日程工具。
    if (operation_service == nullptr) return Status::Ok();

    // 操作记录查询：记录写入不经过 tool，由变更 service 显式推送；本工具只读查询。
    status = server.add_tool(
        "schedule.operation_query", "查询最近的操作记录，支持按对象类型、操作类型和名称筛选。",
        OperationQueryProperties(), [operation_service](const PropertyList& properties) {
            if (operation_service == nullptr) return FailureOutput("当前运行时未启用操作记录能力");

            schedule::QueryOperationCommand command;
            const auto entity_type = properties.value<std::string>("entity_type");
            if (entity_type.has_value()) {
                const auto parsed = ParseEntityType(*entity_type);
                if (!parsed.has_value()) return FailureOutput("entity_type 取值为 schedule、rule、exception");
                command.entity_type = parsed;
            }
            const auto type = properties.value<std::string>("type");
            if (type.has_value()) {
                const auto parsed = ParseOperationType(*type);
                if (!parsed.has_value()) return FailureOutput("type 取值为 create、update、delete");
                command.type = parsed;
            }
            command.keyword = properties.value<std::string>("keyword");
            // 最近 15 分钟窗口由 handler 作为调用方约定填充，分页取最近 50 条。
            const DateTime now = Now();
            command.operated_from = now - std::chrono::minutes{15};
            command.operated_to = now;
            command.limit = 50;
            command.offset = 0;

            const auto result = operation_service->query_operations(command);
            if (!result.result.ok()) return FailureOutput(result.result.status.message);
            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String("query success")),
                MakeToolOutput("total", ToolOutputValue::Integer(result.total)),
                MakeToolOutput("operations",
                               ToolOutputValue::Array(schedule_tool_output::OperationArrayOutput(result.result.value))),
            });
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule.reminder_acknowledge",
        "当用户明确确认已获知提醒内容（如‘我知道了’、‘好的’、‘收到’等）时调用。批量处理最近 10 "
        "分钟内所有已触发但未确认的提醒，关闭后续重复提醒，并将对应日程标记为已完成。一次性全部处理。",
        PropertyList{}, [reminder_service, reporting_context](const PropertyList&) {
            if (reminder_service == nullptr) return FailureOutput("当前运行时未启用提醒能力");
            const auto result =
                reminder_service->ExecuteRecentReminderActions(schedule::ScheduleReminderActionKind::kAcknowledge);
            if (!result.ok()) return FailureOutput(result.status.message);
            ToolOutputArray events;
            for (const auto& action_result : *result.value) {
                for (const auto& event : action_result.events) {
                    events.emplace_back(MakeToolOutput(ToolOutputValue::String(event)));
                }
            }
            const bool reported = ReportVoiceActionResults(*result.value, reporting_context);
            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String("已确认提醒")),
                MakeToolOutput("affected_count", ToolOutputValue::Integer(static_cast<int64_t>(result.value->size()))),
                MakeToolOutput("events", ToolOutputValue::Array(std::move(events))),
                MakeToolOutput("im_delivery", ToolOutputValue::String(reported ? "submitted" : "retryable_failed")),
            });
        });
    if (!status.ok()) return status;

    return server.add_tool(
        "schedule.reminder_snooze",
        "当用户在语音交互中表达延迟提醒的意图（如‘稍后提醒’、‘过会儿再说’、‘等会儿提醒我’等）时调用。为当前已触发提醒单"
        "独注册一次新的稍后提醒。",
        PropertyList{}, [reminder_service, reporting_context](const PropertyList&) {
            if (reminder_service == nullptr) return FailureOutput("当前运行时未启用提醒能力");
            const auto result =
                reminder_service->ExecuteRecentReminderActions(schedule::ScheduleReminderActionKind::kSnooze);
            if (!result.ok()) return FailureOutput(result.status.message);
            const bool reported = ReportVoiceActionResults(*result.value, reporting_context);
            return Output({
                MakeToolOutput("status", ToolOutputValue::String("success")),
                MakeToolOutput("message", ToolOutputValue::String("已延迟提醒")),
                MakeToolOutput("affected_count", ToolOutputValue::Integer(static_cast<int64_t>(result.value->size()))),
                MakeToolOutput("im_delivery", ToolOutputValue::String(reported ? "submitted" : "retryable_failed")),
            });
        });
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service) {
    return RegisterScheduleMcpTools(server, service, nullptr, nullptr, nullptr, {});
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService& rule_service) {
    return RegisterScheduleMcpTools(server, service, &rule_service, nullptr, nullptr, {});
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService& rule_service,
                                schedule::ScheduleOperationService& operation_service) {
    return RegisterScheduleMcpTools(server, service, &rule_service, &operation_service, nullptr, {});
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService& rule_service,
                                schedule::ScheduleOperationService& operation_service,
                                schedule::ScheduleReminderService* reminder_service) {
    return RegisterScheduleMcpTools(server, service, &rule_service, &operation_service, reminder_service, {});
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService& rule_service,
                                schedule::ScheduleOperationService& operation_service,
                                schedule::ScheduleReminderService* reminder_service,
                                ScheduleQueryReportingContext reporting_context) {
    return RegisterScheduleMcpTools(server, service, &rule_service, &operation_service, reminder_service,
                                    std::move(reporting_context));
}

}  // namespace voicelife::mcp
