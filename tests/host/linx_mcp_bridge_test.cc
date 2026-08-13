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
    const auto successful_outcome = voicelife::runtime::InspectLinxMcpToolOutcome(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":"创建会议","start_time":1900000000}},"id":3})",
        call);
    Check(successful_outcome.success && successful_outcome.summary == "日程已创建",
          "成功 MCP 机器结果不得进入用户可见会话/屏幕语义");

    const auto initialized_notification = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})", server, "remote-session");
    Check(initialized_notification.ok() && initialized_notification.value.has_value() &&
              initialized_notification.value->empty(),
          "MCP initialized 通知必须被消费且不回包");

    const auto missing = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"unknown.tool","arguments":{}},"id":4})", server);
    Check(missing.ok(), "未知工具必须返回 JSON-RPC 错误响应");
    const auto& missing_result = ParseMcpEnvelope(*missing.value);
    Check(missing_result.Get("error")->Get("code")->number == -32601, "未知工具应回传 JSON-RPC method-not-found");
    const auto missing_outcome = voicelife::runtime::InspectLinxMcpToolOutcome(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"unknown.tool","arguments":{}},"id":4})", missing);
    Check(!missing_outcome.success && missing_outcome.summary == "操作失败",
          "合法 JSON-RPC 未知工具错误不得向用户泄露诊断信息");

    const auto invalid_arguments = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":42}},"id":5})",
        server);
    Check(invalid_arguments.ok(), "非法工具参数必须回传 JSON-RPC 错误帧");
    const auto invalid_outcome = voicelife::runtime::InspectLinxMcpToolOutcome(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":42}},"id":5})",
        invalid_arguments);
    Check(!invalid_outcome.success && invalid_outcome.summary == "日程创建失败",
          "非法参数错误不得进入用户可见 MCP 摘要");

    const auto schedules_before_unavailable =
        service.query_schedule({.status = voicelife::schedule::ScheduleStatusFilter::kAll});
    const auto unavailable = voicelife::runtime::BuildLinxMcpUnavailableResponse(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":"不应执行"}},"id":"busy-1"})",
        "设备 MCP 正忙，请稍后重试", "remote-session");
    Check(unavailable.ok(), "有界 worker 满载时必须能回传受控 JSON-RPC 错误");
    const auto& unavailable_result = ParseMcpEnvelope(*unavailable.value);
    Check(unavailable_result.Get("id")->string == "busy-1" &&
              unavailable_result.Get("error")->Get("code")->number == -32001,
          "MCP 忙响应必须保留请求 id 并使用稳定的 server-error code");
    const auto schedules_after_unavailable =
        service.query_schedule({.status = voicelife::schedule::ScheduleStatusFilter::kAll});
    Check(schedules_after_unavailable.schedules.size() == schedules_before_unavailable.schedules.size(),
          "构建 busy 响应不得执行任何日程工具");

    const auto notification_busy = voicelife::runtime::BuildLinxMcpUnavailableResponse(
        R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})", "ignored", "remote-session");
    Check(notification_busy.ok() && notification_busy.value->empty(), "通知在 MCP worker 不可用时也不得错误回包");
    return 0;
}
