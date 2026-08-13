#pragma once

#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {
class McpServer;
}

namespace voicelife::runtime {

/** @brief 已解析的 MCP tools/call 用户语义结果。 */
struct LinxMcpToolOutcome {
    bool success = false;
    std::string summary = "日程操作失败";
};

/** @brief 处理 Linx MCP JSON-RPC payload，并返回带会话标识的 type=mcp 响应。 */
Result<std::string> HandleLinxMcpPayload(std::string_view payload, const mcp::McpServer& server,
                                         std::string_view session_id = {});

/**
 * @brief 为已解析但未执行的 MCP 请求生成受控错误响应。
 *
 * 供 Runtime 的有界 MCP worker 队列满载或超时时使用。该函数只解析
 * JSON-RPC 信封以保留请求 id，绝不调用工具或 ScheduleService。
 */
Result<std::string> BuildLinxMcpUnavailableResponse(std::string_view payload, std::string_view message,
                                                    std::string_view session_id = {});

/**
 * @brief 从 Linx MCP 响应信封提取 tools/call 的成功语义与显示摘要。
 *
 * JSON-RPC 业务错误也是合法的响应帧，不能仅凭 Result::ok() 判断成功。
 * 此函数只解析受控信封，不调用工具、Provider 或显示端口。
 */
LinxMcpToolOutcome InspectLinxMcpToolOutcome(const Result<std::string>& response);

}  // namespace voicelife::runtime
