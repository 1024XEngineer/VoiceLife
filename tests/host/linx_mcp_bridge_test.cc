#include "linx_mcp_bridge.h"

#include "schedule_mcp_tools.h"
#include "support/test_support.h"
#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::mcp::McpServer;
using voicelife::schedule::ScheduleService;
using voicelife::test::Check;

namespace {

voicelife::JsonValue ParseMcpEnvelope(const std::string& encoded) {
    voicelife::JsonValue envelope;
    Check(voicelife::ParseJson(encoded, envelope).ok(), "MCP 响应必须是合法 JSON");
    Check(envelope.IsObject() && envelope.Get("type") != nullptr && envelope.Get("type")->string == "mcp",
          "Linx MCP 响应必须使用 mcp 信封");
    const voicelife::JsonValue* payload = envelope.Get("payload");
    Check(payload != nullptr && payload->IsObject(), "Linx MCP 信封必须包含对象 payload");
    return *payload;
}

}  // namespace

int main() {
    McpServer server;
    ScheduleService service;
    Check(voicelife::runtime::RegisterScheduleMcpTools(server, service).ok(), "测试前应注册日程工具");

    const auto initialize =
        voicelife::runtime::HandleLinxMcpPayload(R"({"jsonrpc":"2.0","method":"initialize","id":1})", server);
    Check(initialize.ok(), "initialize 应返回设备能力");
    const auto& initialized = ParseMcpEnvelope(*initialize.value);
    Check(initialized.Get("jsonrpc")->string == "2.0" && initialized.Get("id")->number == 1,
          "initialize 必须保留 JSON-RPC 版本和请求 ID");
    Check(initialized.Get("result")->Get("protocolVersion")->string == "2024-11-05" &&
              initialized.Get("result")->Get("capabilities")->Get("tools")->IsObject(),
          "initialize 必须声明 MCP tools 能力");

    const auto list = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/list","id":"list-1"})", server, "remote-session");
    Check(list.ok(), "tools/list 应返回可发现的日程工具和 Schema");
    const auto& listed = ParseMcpEnvelope(*list.value);
    Check(list.value->find("\"session_id\":\"remote-session\"") != std::string::npos,
          "MCP 响应必须回传 Linx session_id");
    const auto& tools = listed.Get("result")->Get("tools")->array;
    Check(tools.size() == 2 && tools[0].Get("name")->string == "schedule.create" &&
              tools[1].Get("name")->string == "schedule.query",
          "tools/list 必须返回两个稳定排序的 MVP 工具");
    const auto* create_schema = tools[0].Get("inputSchema");
    Check(create_schema->Get("required")->array.size() == 1 &&
              create_schema->Get("required")->array[0].string == "event" &&
              create_schema->Get("properties")->Get("start_time")->Get("type")->string == "integer" &&
              create_schema->Get("properties")->Get("start_time")->Get("default") == nullptr,
          "可选日程时间不得被伪造成带默认值的必填参数");

    const auto call = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":"创建会议","start_time":1900000000}},"id":3})",
        server);
    Check(call.ok(), "tools/call 应分发给日程工具并回传文本结果");
    const auto& called = ParseMcpEnvelope(*call.value);
    Check(called.Get("result")->Get("content")->array.size() == 1 &&
              called.Get("result")->Get("content")->array[0].Get("type")->string == "text" &&
              called.Get("result")->Get("content")->array[0].Get("text")->string.find("event=创建会议") !=
                  std::string::npos,
          "tools/call 必须返回 MCP text content");
    Check(called.Get("result")->Get("isError")->boolean == false, "成功 tools/call 必须明确声明 isError=false");

    const auto missing = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"unknown.tool","arguments":{}},"id":4})", server);
    Check(missing.ok(), "未知工具必须返回 JSON-RPC 错误响应");
    const auto& missing_result = ParseMcpEnvelope(*missing.value);
    Check(missing_result.Get("error")->Get("code")->number == -32601, "未知工具应回传 JSON-RPC method-not-found");
    return 0;
}
