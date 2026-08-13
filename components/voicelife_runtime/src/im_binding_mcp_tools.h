#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {
class McpServer;
}

namespace voicelife::im {
class BindingUseCase;
}

namespace voicelife::runtime {

/** @brief 向 MCP Server 注册微信公众号绑定工具。 */
Status RegisterImBindingMcpTools(mcp::McpServer& server, im::BindingUseCase& use_case);

}  // namespace voicelife::runtime
