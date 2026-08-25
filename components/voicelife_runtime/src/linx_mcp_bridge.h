#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {
class McpServer;
}

namespace voicelife::runtime {

/// 单个 Linx MCP 文本信封的保守上行安全上限。
inline constexpr std::size_t kLinxMcpMaxResponseBytes = 3000;

/** @brief 已解析的 MCP tools/call 用户可见语义结果。 */
struct LinxMcpToolOutcome {
    bool success = false;
    std::string summary = "操作失败";
    std::string result_status = "unknown";
};

/** @brief 处理 Linx MCP JSON-RPC payload，并返回带会话标识的 type=mcp 响应。 */
Result<std::string> HandleLinxMcpPayload(std::string_view payload, const mcp::McpServer& server,
                                         std::string_view session_id = {}, LinxMcpToolOutcome* tool_outcome = nullptr);

/**
 * @brief 为已解析但未执行的 MCP 请求生成受控错误响应。
 *
 * 供 Runtime 的有界 MCP worker 队列满载或超时时使用。该函数只解析
 * JSON-RPC 信封以保留请求 id，绝不调用工具或 ScheduleService。
 */
Result<std::string> BuildLinxMcpUnavailableResponse(std::string_view payload, std::string_view message,
                                                    std::string_view session_id = {});

/**
 * @brief 从受控 tools/call 请求和 Linx 响应提取用户可见业务语义。
 *
 * JSON-RPC 业务错误也是合法的响应帧，不能仅凭 Result::ok() 判断成功。
 * 仅根据已注册工具名映射稳定业务文案，例如“日程已创建”或“日程查询
 * 失败”。若响应仍保留结构化 status，会同步提取受控状态。不得携带 MCP
 * 的机器结果、服务端错误原文或参数校验细节；此函数只解析受控信封，不调用
 * 工具、Provider 或显示端口。
 */
LinxMcpToolOutcome InspectLinxMcpToolOutcome(std::string_view request_payload, const Result<std::string>& response);

/** @brief 绑定工具已有独立 OLED 呈现，通用 MCP 结果层不得再覆盖它。 */
bool IsBindingMcpToolSummary(std::string_view summary);

}  // namespace voicelife::runtime
