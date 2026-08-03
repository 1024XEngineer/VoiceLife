#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "voicelife/mcp/tool_definition.h"

namespace voicelife::mcp {

// 管理工具定义与执行回调，并负责将模型调用分发给对应工具。
class McpToolGateway {
   public:
    // 注册工具定义和回调；同名工具已存在时不会覆盖原注册项。
    Status register_tool(ToolDefinition definition, ToolHandler handler);

    // 根据工具名称查询可公开的工具定义。
    [[nodiscard]] GetToolResult get_tool(std::string_view name) const;

    // 按注册顺序返回全部可公开的工具定义。
    [[nodiscard]] ListToolsResult list_tools() const;

    // 根据调用中的工具名称执行已注册的 handler。
    ToolResult call(const ToolCall& call) const;

   private:
    // handler 仅供本地执行，不作为 ToolDefinition 的一部分向模型导出。
    struct RegisteredTool {
        ToolDefinition definition;
        ToolHandler handler;
    };

    std::unordered_map<std::string, RegisteredTool> tools_;
    // unordered_map 不保证遍历顺序，因此单独记录注册顺序以稳定导出结果。
    std::vector<std::string> registration_order_;
};

}  // namespace voicelife::mcp
