#pragma once

#include <functional>

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {

class McpServer;

/**
 * @brief 注册官方 SparkBot 兼容的扬声器音量 MCP 工具。
 *
 * 设备层只提供设置回调，MCP 模块负责固定工具名、参数范围和返回形状，
 * 这样 host 测试可以在不依赖 ESP 音频硬件的情况下覆盖完整契约。
 */
[[nodiscard]] Status RegisterAudioMcpTools(McpServer& server, std::function<void(int)> set_volume);

}  // namespace voicelife::mcp
