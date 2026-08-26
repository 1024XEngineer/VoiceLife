#define main ExistingMcpJsonWriterCoverageTestMain
#include "mcp_json_writer_coverage_test.cc"
#undef main

#define main ExistingLinxMcpBridgeTestMain
#include "linx_mcp_bridge_test.cc"
#undef main

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::ToolOutputValue;
using voicelife::ToolResult;
using voicelife::mcp::Property;
using voicelife::mcp::PropertyList;
using voicelife::mcp::PropertyType;
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

    const auto numeric_cursor = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/list","params":{"cursor":1},"id":"cursor-number"})", server);
    Check(numeric_cursor.ok() && numeric_cursor.value->find("\"code\":-32602") != std::string::npos,
          "非字符串 tools/list cursor 必须返回 invalid params");
    const auto out_of_range_cursor = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/list","params":{"cursor":"1"},"id":"cursor-range"})", server);
    Check(out_of_range_cursor.ok() && out_of_range_cursor.value->find("\"code\":-32602") != std::string::npos,
          "超出目录范围的 tools/list cursor 必须返回 invalid params");

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

void CheckAdditionalProtocolBranches() {
    McpServer server;
    voicelife::test::InMemoryScheduleRepository repository;
    voicelife::schedule::ScheduleService service(repository);
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service).ok(), "协议边界测试应注册日程工具");

    const auto initialized = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"initialize","id":"init-1"})", server, "session\n\"id");
    Check(initialized.ok() && initialized.value->find("session\\n\\\"id") != std::string::npos,
          "initialize 应转义并回传特殊 session_id");

    const auto notification =
        voicelife::runtime::HandleLinxMcpPayload(R"({"jsonrpc":"2.0","method":"notifications/progress"})", server);
    Check(notification.ok() && notification.value->empty(), "任意 notifications/ 通知都不应回包");

    const auto list_with_boolean_id =
        voicelife::runtime::HandleLinxMcpPayload(R"({"jsonrpc":"2.0","method":"tools/list","id":true})", server);
    Check(list_with_boolean_id.ok() && list_with_boolean_id.value->find("\"id\":true") != std::string::npos,
          "非字符串 JSON-RPC id 应按原类型序列化");

    const auto missing_arguments = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create"},"id":7})", server);
    Check(missing_arguments.ok() && missing_arguments.value->find("-32602") != std::string::npos,
          "缺少必填工具参数应返回 invalid params");

    const auto null_params = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":null,"id":8})", server);
    Check(null_params.ok() && null_params.value->find("-32602") != std::string::npos,
          "null params 应返回 invalid params");

    const auto failed_response = voicelife::Result<std::string>::Failure(ErrorCode::kUnavailable, "transport");
    const auto failed_outcome = voicelife::runtime::InspectLinxMcpToolOutcome(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"im.binding.start"},"id":1})", failed_response);
    Check(!failed_outcome.success && failed_outcome.summary == "绑定操作失败", "传输失败应返回绑定失败摘要");

    const auto non_object_envelope = voicelife::Result<std::string>::Success("[]");
    Check(!voicelife::runtime::InspectLinxMcpToolOutcome("{}", non_object_envelope).success,
          "非对象响应信封不得判定成功");
    const auto missing_payload = voicelife::Result<std::string>::Success(R"({"type":"mcp"})");
    Check(!voicelife::runtime::InspectLinxMcpToolOutcome("{}", missing_payload).success,
          "缺少 payload 的响应不得判定成功");
    const auto non_object_result =
        voicelife::Result<std::string>::Success(R"({"type":"mcp","payload":{"result":null}})");
    Check(!voicelife::runtime::InspectLinxMcpToolOutcome("{}", non_object_result).success,
          "非对象 result 不得判定成功");
    const auto invalid_is_error =
        voicelife::Result<std::string>::Success(R"({"type":"mcp","payload":{"result":{"isError":"false"}}})");
    Check(!voicelife::runtime::InspectLinxMcpToolOutcome("{}", invalid_is_error).success,
          "非布尔 isError 不得判定成功");
}

void CheckProtocolSerializationBranches() {
    McpServer server;
    Check(server
              .add_tool("protocol.echo", "协议序列化回显",
                        PropertyList({Property::Optional("text", PropertyType::kString),
                                      Property::Optional("enabled", PropertyType::kBoolean),
                                      Property::Optional("count", PropertyType::kInteger),
                                      Property::Optional("metadata", PropertyType::kObject)}),
                        [](const PropertyList& properties) {
                            (void)properties;
                            ToolResult result = ToolResult::Success(ToolOutputValue::String("ignored"));
                            result.text_output = "quote:\" slash:\\ control:\b\f\n\r\t\x01";
                            return result;
                        })
              .ok(),
          "协议序列化测试工具应注册成功");

    const std::string session_id = "quote:\" slash:\\ control:\b\f\n\r\t\x01";
    const auto list = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/list","id":{"request":"list","items":[null,false,2]}})", server,
        session_id);
    Check(list.ok(), "对象和数组 request id 的 tools/list 应成功");
    voicelife::JsonValue list_response;
    Check(voicelife::ParseJson(*list.value, list_response).ok(), "复合 request id 和 session id 必须正确转义");

    const auto call = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"protocol.echo","arguments":{"text":"value","enabled":true,"count":7,"metadata":{"nested":"value"}}},"id":[null,true,false,7,"call"]})",
        server, session_id);
    Check(call.ok(), "全部支持参数类型的 tools/call 应成功");
    voicelife::JsonValue call_response;
    Check(voicelife::ParseJson(*call.value, call_response).ok(), "含控制字符的工具文本必须正确转义");
}

}  // namespace

int main() {
    Check(ExistingMcpJsonWriterCoverageTestMain() == 0, "MCP JSON Writer 覆盖测试应通过");
    Check(ExistingLinxMcpBridgeTestMain() == 0, "完整 Linx MCP 桥接测试应通过");
    CheckBridgeProtocolFailures();
    CheckUnavailableAndOutcomeBranches();
    CheckAdditionalProtocolBranches();
    CheckProtocolSerializationBranches();
    return 0;
}
