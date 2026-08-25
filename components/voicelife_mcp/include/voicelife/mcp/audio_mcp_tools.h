#pragma once

#include <functional>

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {

/** @brief MCP server 的前置声明，避免音量工具头文件引入完整实现。 */
class McpServer;

/**
 * @brief 注册官方 SparkBot 兼容的扬声器音量 MCP 工具。
 *
 * 设备层只提供设置回调，MCP 模块负责固定工具名、参数范围和返回形状，
 * 这样 host 测试可以在不依赖 ESP 音频硬件的情况下覆盖完整契约。
 * @param server 要注册音量工具的 MCP server。
 * @param int 音量回调接收的整数类型，取值范围为 0 到 100。
 * @param set_volume 设备层音量设置回调。
 * @return 注册成功返回 OK，否则返回参数或注册错误。
 */
[[nodiscard]] Status RegisterAudioMcpTools(McpServer& server, std::function<void(int)> set_volume);

}  // namespace voicelife::mcp
