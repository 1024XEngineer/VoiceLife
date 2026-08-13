#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::schedule {
class ScheduleRuleService;
}

namespace voicelife::mcp {

class McpServer;

/** @brief 向 MCP Server 注册周期规则相关的日程工具。 */
Status RegisterScheduleRuleMcpTools(McpServer& server, schedule::ScheduleRuleService& service);

}  // namespace voicelife::mcp
