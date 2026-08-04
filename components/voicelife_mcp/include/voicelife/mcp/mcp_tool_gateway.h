#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "voicelife/mcp/tool_definition.h"

namespace voicelife::mcp {

/// 管理工具定义，并把模型调用分发给已注册的处理器。
class McpToolGateway {
   public:
    /**
     * @brief 注册工具定义和处理器，不覆盖已有同名工具。
     * @param definition 要注册的公开工具契约。
     * @param handler 执行工具的本地回调。
     * @return 注册结果；重复名称会被拒绝。
     */
    Status register_tool(ToolDefinition definition, ToolHandler handler);

    /**
     * @brief 按名称查询公开工具定义。
     * @param name 已注册工具的名称。
     * @return 带 found 标记和可选定义的查询结果。
     */
    [[nodiscard]] GetToolResult get_tool(std::string_view name) const;

    /** @brief 按注册顺序返回公开工具。 @return 全部已注册的公开工具。 */
    [[nodiscard]] ListToolsResult list_tools() const;

    /**
     * @brief 执行工具调用对应的处理器。
     * @param call 要分发的工具调用。
     * @return 工具调用的语义化结果。
     */
    ToolResult call(const ToolCall& call) const;

   private:
    /// 保存公开定义和不对外导出的本地处理器。
    struct RegisteredTool {
        ToolDefinition definition;
        ToolHandler handler;
    };

    std::unordered_map<std::string, RegisteredTool> tools_;
    /// unordered_map 不保证遍历顺序，因此单独记录注册顺序以稳定导出结果。
    std::vector<std::string> registration_order_;
};

}  // namespace voicelife::mcp
