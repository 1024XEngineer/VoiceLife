#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::schedule {
/// 提供周期日程规则服务能力。
class ScheduleRuleService;
}  // namespace voicelife::schedule

namespace voicelife::mcp {

/// 用于注册周期规则 MCP 工具的 MCP Server 前向声明。
class McpServer;

/**
 * @brief 向 MCP Server 注册周期规则相关的日程工具。
 * @param server 要注册工具的 MCP Server。
 * @param service 周期日程规则服务。
 * @return 注册结果。
 */
Status RegisterScheduleRuleMcpTools(McpServer& server, schedule::ScheduleRuleService& service);

}  // namespace voicelife::mcp
