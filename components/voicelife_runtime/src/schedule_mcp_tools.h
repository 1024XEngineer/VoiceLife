#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {
class McpServer;
}

namespace voicelife::schedule {
class ScheduleService;
}

namespace voicelife::runtime {

/** @brief 向 MCP Server 注册当前 MVP 的日程工具。 */
Status RegisterScheduleMcpTools(mcp::McpServer& server, schedule::ScheduleService& service);

}  // namespace voicelife::runtime
