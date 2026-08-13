#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/mcp/json_rpc_endpoint.h"
#include "voicelife/mcp/mcp_request_executor.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_service.h"

namespace voicelife::schedule {
class ScheduleRepository;
}

namespace voicelife::mcp {

/**
 * @brief 为日程用例装配 MCP Server、工具注册和 JSON-RPC endpoint。
 *
 * Runtime 只能构造此应用并将 HandleJsonRpc 注入传输 Adapter；工具定义、
 * 参数映射和业务路由均留在 MCP 组件。
 */
class McpScheduleApplication final {
   public:
    using ExecutionObserver = std::function<void(bool started, bool success, std::string_view summary)>;

    explicit McpScheduleApplication(schedule::ScheduleRepository& repository);
    ~McpScheduleApplication();

    McpScheduleApplication(const McpScheduleApplication&) = delete;
    McpScheduleApplication& operator=(const McpScheduleApplication&) = delete;

    [[nodiscard]] Status Initialize();
    /**
     * @brief 异步执行 MCP 请求；传输回调只允许调用此入口。
     *
     * 响应保持纯 JSON-RPC，传输信封由调用方所属的协议 Adapter 负责。
     */
    [[nodiscard]] Status SubmitJsonRpc(std::string_view request, McpJsonRpcResponseSink response_sink);
    void SetExecutionObserver(ExecutionObserver observer);

   private:
    [[nodiscard]] Result<std::string> ExecuteJsonRpc(std::string_view request) const;
    [[nodiscard]] std::string UserSummary(std::string_view request, const Result<std::string>& response) const;
    [[nodiscard]] bool ResponseSucceeded(const Result<std::string>& response) const;
    [[nodiscard]] bool IsToolCall(std::string_view request) const;

    McpServer server_;
    schedule::ScheduleService schedule_service_;
    JsonRpcEndpoint endpoint_{server_};
    McpRequestExecutor executor_{[this](std::string_view request) { return ExecuteJsonRpc(request); }};
    ExecutionObserver observer_;
    bool initialized_ = false;
};

}  // namespace voicelife::mcp
