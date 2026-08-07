#pragma once

#include <string>

#include "voicelife/mcp/mcp_server.h"

namespace voicelife::mcp {

/**
 * @brief 将工具列表序列化为 MCP tools/list 的 result JSON。
 * @param result 待序列化的工具列表。
 * @return 序列化成功时返回 JSON 文本，失败时返回空对象。
 */
std::string SerializeListToolsResult(const ListToolsResult& result);

}  // namespace voicelife::mcp
