#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {

/** @brief MCP 请求完成后返回纯 JSON-RPC payload 的回调。 */
using McpJsonRpcResponseSink = std::function<void(Result<std::string>)>;

/** @brief MCP 请求的非实时执行函数。 */
using McpJsonRpcHandler = std::function<Result<std::string>(std::string_view)>;

/**
 * @brief 在专属、有界工作上下文中执行 MCP JSON-RPC 请求。
 *
 * 传输回调只调用 Submit()；工具路由、日程服务和持久化访问始终在该执行器的
 * 工作任务中运行。该类不认识 Linx、语音会话、显示或具体存储实现。
 */
class McpRequestExecutor final {
   public:
    explicit McpRequestExecutor(McpJsonRpcHandler handler);
    ~McpRequestExecutor();

    McpRequestExecutor(const McpRequestExecutor&) = delete;
    McpRequestExecutor& operator=(const McpRequestExecutor&) = delete;

    /** @brief 创建专属 MCP 工作任务。 */
    [[nodiscard]] Status Start();
    /** @brief 停止接收新请求，并释放工作任务。 */
    void Stop();
    /** @brief 将请求投递到有界队列；满时返回 kUnavailable。 */
    [[nodiscard]] Status Submit(std::string_view request, McpJsonRpcResponseSink response_sink);

   private:
    class Impl;
    Impl* impl_;
};

}  // namespace voicelife::mcp
