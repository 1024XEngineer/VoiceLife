#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::schedule {
class ScheduleService;
class ScheduleRuleService;
}

namespace voicelife::mcp {

class McpServer;

/** @brief 向 MCP Server 注册当前日程工具。 */
Status RegisterScheduleMcpTools(McpServer& server, schedule::ScheduleService& service);

/** @brief 向 MCP Server 注册包含周期日程能力的日程工具。 */
Status RegisterScheduleMcpTools(McpServer& server, schedule::ScheduleService& service,
                                schedule::ScheduleRuleService& rule_service);

}  // namespace voicelife::mcp
