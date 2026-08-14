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

/// 新绑定会话创建成功后的脱敏结果回调；Runtime 据此启动轮询并投递设备呈现语义。
using BindingResultHook = std::function<void(const im::BindingResult&)>;

/**
 * @brief 向 MCP Server 注册 IM 平台绑定工具 im.binding.start。
 * @param server 目标 MCP Server。
 * @param use_case 绑定用例；Start/Poll 与 Runtime 任务并发调用，内部已加锁。
 * @param on_result 创建成功后的钩子；未提供时仅返回 MCP 结果、不启动轮询或设备呈现。
 * @return 注册结果。
 */
Status RegisterImBindingMcpTools(mcp::McpServer& server, im::BindingUseCase& use_case,
                                 BindingResultHook on_result = {});

}  // namespace voicelife::runtime
