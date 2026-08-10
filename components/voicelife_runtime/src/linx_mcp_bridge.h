#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {
class McpServer;
}

namespace voicelife::runtime {

/** @brief 处理 Linx MCP JSON-RPC payload，并返回带会话标识的 type=mcp 响应。 */
Result<std::string> HandleLinxMcpPayload(std::string_view payload, const mcp::McpServer& server,
                                         std::string_view session_id = {});

}  // namespace voicelife::runtime
