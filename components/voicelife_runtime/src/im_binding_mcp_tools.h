#pragma once

#include <functional>

#include "voicelife/contracts/status.h"
#include "voicelife/im/im_binding_use_case.h"

namespace voicelife::mcp {
class McpServer;
}

namespace voicelife::im {
class BindingUseCase;
}

namespace voicelife::runtime {

/** @brief 绑定状态 → 稳定机器可读名称（pending/confirmed/expired/...）。 */
const char* BindingStatusName(im::BindingState state);

/// 绑定会话创建成功（pending）后的回调；Runtime 借此启动有界后台轮询，
/// 轮询到 confirmed/expired/cancelled 等终态后释放会话。
using BindingSessionStartedHook = std::function<void()>;

/**
 * @brief 向 MCP Server 注册 IM 平台绑定工具 im.binding.start。
 * @param server 目标 MCP Server。
 * @param use_case 绑定用例；Start/Poll 与 Runtime 任务并发调用，内部已加锁。
 * @param on_session_started 会话创建成功后的钩子；未提供时仅返回结果、不启动轮询。
 * @return 注册结果。
 */
Status RegisterImBindingMcpTools(mcp::McpServer& server, im::BindingUseCase& use_case,
                                 BindingSessionStartedHook on_session_started = {});

}  // namespace voicelife::runtime
