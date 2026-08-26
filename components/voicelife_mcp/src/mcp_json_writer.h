#pragma once

#include <cstddef>
#include <string>

#include "voicelife/mcp/mcp_server.h"

namespace voicelife::mcp {

/**
 * @brief 将工具列表序列化为 MCP tools/list 的 result JSON。
 * @param result 待序列化的工具列表。
 * @return 序列化成功时返回 JSON 文本，失败时返回空对象。
 */
std::string SerializeListToolsResult(const ListToolsResult& result);

/**
 * @brief 将工具列表的连续页序列化为 MCP tools/list 的 result JSON。
 * @param result 按注册顺序排列的完整工具列表。
 * @param begin_index 当前页第一个工具的下标。
 * @param end_index 当前页最后一个工具之后的下标。
 * @return 序列化成功时返回带 nextCursor 的 JSON 文本，失败时返回空对象。
 */
std::string SerializeListToolsResultPage(const ListToolsResult& result, std::size_t begin_index, std::size_t end_index);

}  // namespace voicelife::mcp
