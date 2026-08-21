#define main ExistingLinxMcpBridgeTestMain
#include "linx_mcp_bridge_test.cc"
#undef main

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::test::Check;

namespace {

void CheckBridgeProtocolFailures() {
    McpServer server;

    const auto invalid_json = voicelife::runtime::HandleLinxMcpPayload("not-json", server);
    Check(!invalid_json.ok() && invalid_json.status.code == ErrorCode::kInvalidArgument,
          "非法 JSON 应返回 invalid argument");

    const auto missing_method = voicelife::runtime::HandleLinxMcpPayload(R"({"jsonrpc":"2.0","id":1})", server);
    Check(!missing_method.ok() && missing_method.status.code == ErrorCode::kInvalidArgument,
          "缺少 method 应返回 invalid argument");

    const auto missing_id =
        voicelife::runtime::HandleLinxMcpPayload(R"({"jsonrpc":"2.0","method":"tools/list"})", server);
    Check(!missing_id.ok() && missing_id.status.code == ErrorCode::kInvalidArgument,
          "非通知请求缺少 id 应返回 invalid argument");

    const auto ping = voicelife::runtime::HandleLinxMcpPayload(R"({"jsonrpc":"2.0","method":"ping"})", server);
    Check(ping.ok() && ping.value.has_value() && ping.value->empty(), "无 id ping 应作为通知消费");

    const auto unknown_method =
        voicelife::runtime::HandleLinxMcpPayload(R"({"jsonrpc":"2.0","method":"unknown","id":"m-1"})", server);
    Check(unknown_method.ok() && unknown_method.value->find("-32601") != std::string::npos,
          "未知方法应返回 method-not-found");

    const auto invalid_params = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":[]},"id":2})", server);
    Check(invalid_params.ok() && invalid_params.value->find("-32602") != std::string::npos,
          "非对象 arguments 应返回 invalid params");

    const auto unsupported_value = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":null}},"id":3})",
        server);
    Check(unsupported_value.ok() && unsupported_value.value->find("-32602") != std::string::npos,
          "不支持的 MCP 参数类型应返回 invalid params");

    const auto fractional_value = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":1.5}},"id":4})",
        server);
    Check(fractional_value.ok() && fractional_value.value->find("-32602") != std::string::npos,
          "非整数数字参数应返回 invalid params");
}

void CheckUnavailableAndOutcomeBranches() {
    McpServer server;

    const auto invalid = voicelife::runtime::BuildLinxMcpUnavailableResponse("not-json", "busy", {});
    Check(!invalid.ok() && invalid.status.code == ErrorCode::kInvalidArgument,
          "busy 响应遇到非法 JSON 应返回 invalid argument");

    const auto missing_method =
        voicelife::runtime::BuildLinxMcpUnavailableResponse(R"({"jsonrpc":"2.0","id":1})", "busy", {});
    Check(!missing_method.ok() && missing_method.status.code == ErrorCode::kInvalidArgument,
          "busy 响应缺少 method 应返回 invalid argument");

    const auto ping =
        voicelife::runtime::BuildLinxMcpUnavailableResponse(R"({"jsonrpc":"2.0","method":"ping"})", "busy", {});
    Check(!ping.ok() && ping.status.code == ErrorCode::kInvalidArgument,
          "无 id 的非通知 busy 请求应返回 invalid argument");

    const auto error_response = voicelife::runtime::BuildLinxMcpUnavailableResponse(
        R"({"jsonrpc":"2.0","method":"tools/call","id":1})", "设备忙\"稍后重试", "session");
    Check(error_response.ok() && error_response.value->find("session_id") != std::string::npos &&
              error_response.value->find("-32001") != std::string::npos,
          "busy 错误响应应保留 session 和转义消息");

    const auto malformed_response = Result<std::string>::Success("not-json");
    const auto malformed_outcome = voicelife::runtime::InspectLinxMcpToolOutcome("bad", malformed_response);
    Check(!malformed_outcome.success && malformed_outcome.summary == "操作失败", "无法解析请求时应返回通用失败摘要");

    const auto error_payload =
        Result<std::string>::Success(R"({"type":"mcp","payload":{"jsonrpc":"2.0","id":1,"error":{"code":-32602}}})");
    const auto error_outcome = voicelife::runtime::InspectLinxMcpToolOutcome(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.query"},"id":1})", error_payload);
    Check(!error_outcome.success && error_outcome.summary == "日程查询失败", "MCP error payload 不应暴露诊断内容");

    const auto no_result = Result<std::string>::Success(R"({"type":"mcp","payload":{}})");
    const auto no_result_outcome = voicelife::runtime::InspectLinxMcpToolOutcome("{}", no_result);
    Check(!no_result_outcome.success, "缺少 result 时不得判定成功");

    const auto is_error = Result<std::string>::Success(R"({"type":"mcp","payload":{"result":{"isError":true}}})");
    const auto is_error_outcome = voicelife::runtime::InspectLinxMcpToolOutcome("{}", is_error);
    Check(!is_error_outcome.success, "isError=true 时不得判定成功");

    const auto query_success = Result<std::string>::Success(R"({"type":"mcp","payload":{"result":{"isError":false}}})");
    const auto query_outcome = voicelife::runtime::InspectLinxMcpToolOutcome(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.query"},"id":1})", query_success);
    Check(query_outcome.success && query_outcome.summary == "日程查询完成", "成功 query 应返回查询摘要");
    Check(!voicelife::runtime::IsBindingMcpToolSummary("操作已完成"), "非绑定摘要不得误判为绑定结果");
}

}  // namespace

int main() {
    CheckBridgeProtocolFailures();
    CheckUnavailableAndOutcomeBranches();
    return 0;
}
