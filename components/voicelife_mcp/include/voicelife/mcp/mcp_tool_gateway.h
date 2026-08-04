#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "voicelife/mcp/tool_definition.h"

namespace voicelife::mcp {

/// Manages tool definitions and dispatches model calls to registered handlers.
class McpToolGateway {
   public:
    /**
     * @brief Registers a tool definition and handler without replacing an existing name.
     * @param definition Public tool contract to register.
     * @param handler Local callback that executes the tool.
     * @return Registration result; duplicate names are rejected.
     */
    Status register_tool(ToolDefinition definition, ToolHandler handler);

    /**
     * @brief Looks up a public tool definition by name.
     * @param name Name of the registered tool.
     * @return Lookup result with a found flag and optional definition.
     */
    [[nodiscard]] GetToolResult get_tool(std::string_view name) const;

    /** @brief Lists public tools in registration order. @return All registered public tools. */
    [[nodiscard]] ListToolsResult list_tools() const;

    /**
     * @brief Executes the handler registered for a tool call.
     * @param call Tool invocation to dispatch.
     * @return Semantic result of the tool invocation.
     */
    ToolResult call(const ToolCall& call) const;

   private:
    /// Holds a public definition and its non-exported local handler.
    struct RegisteredTool {
        ToolDefinition definition;
        ToolHandler handler;
    };

    std::unordered_map<std::string, RegisteredTool> tools_;
    /// unordered_map 不保证遍历顺序，因此单独记录注册顺序以稳定导出结果。
    std::vector<std::string> registration_order_;
};

}  // namespace voicelife::mcp
