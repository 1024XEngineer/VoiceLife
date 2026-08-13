#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {
class McpServer;
}

namespace voicelife::schedule {
class ScheduleService;
}

namespace voicelife::mcp {

/** @brief 将日程 Use Case 注册为 MCP 工具；不持有业务状态或存储资源。 */
Status RegisterScheduleTools(McpServer& server, schedule::ScheduleService& service);

}  // namespace voicelife::mcp
