#pragma once

#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {

/** @brief 提供 MCP 工具注册与调用的服务端。 */
class McpServer;

/**
 * @brief 将受限 JSON-RPC MCP 请求映射到 McpServer。
 *
 * 该类不认识 WebSocket、Linx 信封、数据库或板级资源；传输 Adapter 负责
 * 收发信封，业务 Adapter 负责向 McpServer 注册工具。
 */
class JsonRpcEndpoint final {
   public:
    /** @brief 以已注册工具的 MCP 服务端创建 endpoint。 @param server 工具服务端。 */
    explicit JsonRpcEndpoint(const McpServer& server) : server_(server) {}

    /** @brief 处理 initialize、tools/list、tools/call 和 notification。
     * @param request JSON-RPC 请求。
     * @return JSON-RPC 响应或解析错误。
     */
    [[nodiscard]] Result<std::string> Handle(std::string_view request) const;
    /** @brief 为未进入工具执行的受控拒绝生成 JSON-RPC 错误响应。
     * @param request 原始 JSON-RPC 请求。
     * @param message 受控拒绝说明。
     * @return JSON-RPC 错误响应或解析错误。
     */
    [[nodiscard]] static Result<std::string> UnavailableResponse(std::string_view request, std::string_view message);

   private:
    const McpServer& server_;
};

}  // namespace voicelife::mcp
