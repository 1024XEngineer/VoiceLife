#pragma once

#include <functional>
#include <optional>
#include <string>

#include "voicelife/contracts/status.h"

namespace voicelife::im {
/// 提供 IM Gateway 上报能力的运行时。
class ImRuntime;
}  // namespace voicelife::im

namespace voicelife::schedule {
/// 提供一次性日程服务能力。
class ScheduleService;
/// 提供周期日程规则服务能力。
class ScheduleRuleService;
/// 提供日程操作记录服务能力。
class ScheduleOperationService;
/// 提供日程提醒同步能力。
class ScheduleReminderService;
struct ReminderActionResult;
}  // namespace voicelife::schedule

namespace voicelife::mcp {

/// schedule.query 将完整结果转发到 IM 时使用的设备上下文。
struct ScheduleQueryReportingContext {
    voicelife::im::ImRuntime* runtime = nullptr;
    std::function<Status(const schedule::ReminderActionResult&)> voice_action_reporter;
};

/// 用于注册日程 MCP 工具的 MCP Server 前向声明。
class McpServer;

/**
 * @brief 向 MCP Server 注册当前日程工具。
 * @param server 要注册工具的 MCP Server。
 * @param service 一次性日程服务。
 * @return 注册结果。
 */
Status RegisterScheduleMcpTools(McpServer& server, schedule::ScheduleService& service);

/**
 * @brief 向 MCP Server 注册包含周期日程能力的日程工具。
 * @param server 要注册工具的 MCP Server。
 * @param service 一次性日程服务。
 * @param rule_service 周期日程规则服务。
 * @return 注册结果。
 */
Status RegisterScheduleMcpTools(McpServer& server, schedule::ScheduleService& service,
                                schedule::ScheduleRuleService& rule_service);

/**
 * @brief 向 MCP Server 注册包含周期日程与操作记录查询能力的日程工具。
 * @param server 要注册工具的 MCP Server。
 * @param service 一次性日程服务。
 * @param rule_service 周期日程规则服务。
 * @param operation_service 日程操作记录服务。
 * @return 注册结果。
 */
Status RegisterScheduleMcpTools(McpServer& server, schedule::ScheduleService& service,
                                schedule::ScheduleRuleService& rule_service,
                                schedule::ScheduleOperationService& operation_service);

/**
 * @brief 向 MCP Server 注册包含周期日程、操作记录与提醒同步能力的日程工具。
 * @param server 要注册工具的 MCP Server。
 * @param service 一次性日程服务。
 * @param rule_service 周期日程规则服务。
 * @param operation_service 日程操作记录服务。
 * @param reminder_service 可选日程提醒服务；为空时保留原有无提醒工具行为。
 * @return 注册结果。
 */
Status RegisterScheduleMcpTools(McpServer& server, schedule::ScheduleService& service,
                                schedule::ScheduleRuleService& rule_service,
                                schedule::ScheduleOperationService& operation_service,
                                schedule::ScheduleReminderService* reminder_service);

/**
 * @brief 注册带 IM 查询结果上报能力的日程工具。
 * @param server 要注册工具的 MCP Server。
 * @param service 一次性日程服务。
 * @param rule_service 周期日程规则服务。
 * @param operation_service 日程操作记录服务。
 * @param reminder_service 可选日程提醒服务；为空时保留原有无提醒工具行为。
 * @param reporting_context IM 查询结果上报所需的设备上下文。
 * @return 注册结果。
 */
Status RegisterScheduleMcpTools(McpServer& server, schedule::ScheduleService& service,
                                schedule::ScheduleRuleService& rule_service,
                                schedule::ScheduleOperationService& operation_service,
                                schedule::ScheduleReminderService* reminder_service,
                                ScheduleQueryReportingContext reporting_context);

}  // namespace voicelife::mcp
