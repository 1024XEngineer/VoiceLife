#include "linx_mcp_bridge.h"

#include <optional>
#include <string>
#include <vector>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/mcp/schedule_mcp_tools.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::JsonValue;
using voicelife::mcp::McpServer;
using voicelife::schedule::ScheduleService;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

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

struct ToolListing {
    std::vector<std::string> names;
    std::vector<std::size_t> response_sizes;
    JsonValue first_tool;
};

ToolListing ReadAllToolPages(const McpServer& server, std::string_view session_id) {
    ToolListing listing;
    std::optional<std::string> cursor;
    for (std::size_t page_index = 0; page_index < 16; ++page_index) {
        std::string request =
            "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":\"list-" + std::to_string(page_index) + "\"";
        if (cursor.has_value()) request += ",\"params\":{\"cursor\":\"" + *cursor + "\"}";
        request += "}";

        const auto response = voicelife::runtime::HandleLinxMcpPayload(request, server, session_id);
        Check(response.ok() && response.value->size() <= voicelife::runtime::kLinxMcpMaxResponseBytes,
              "每个 tools/list 的完整 Linx 信封都必须落在安全上限内");
        listing.response_sizes.push_back(response.value->size());
        const JsonValue listed = ParseMcpEnvelope(*response.value);
        const JsonValue* result = listed.Get("result");
        const JsonValue* tools = result == nullptr ? nullptr : result->Get("tools");
        const JsonValue* next_cursor = result == nullptr ? nullptr : result->Get("nextCursor");
        Check(tools != nullptr && tools->IsArray() && next_cursor != nullptr, "每一页必须包含 tools 和 nextCursor");
        for (const auto& tool : tools->array) {
            const JsonValue* name = tool.Get("name");
            Check(name != nullptr && name->IsString(), "目录中的每个工具必须保留名称");
            if (listing.names.empty()) listing.first_tool = tool;
            listing.names.push_back(name->string);
        }
        if (next_cursor->kind == JsonValue::Kind::kNull) return listing;
        Check(next_cursor->IsString() && !next_cursor->string.empty(), "非末页必须返回非空字符串 cursor");
        cursor = next_cursor->string;
    }
    Check(false, "tools/list cursor 未在合理页数内结束");
    return listing;
}

}  // namespace

int main() {
    McpServer server;
    InMemoryScheduleRepository repository;
    ScheduleService service(repository);
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service).ok(), "测试前应注册日程工具");

    const auto initialize =
        voicelife::runtime::HandleLinxMcpPayload(R"({"jsonrpc":"2.0","method":"initialize","id":1})", server);
    Check(initialize.ok(), "initialize 应返回设备能力");
    const auto& initialized = ParseMcpEnvelope(*initialize.value);
    Check(initialized.Get("jsonrpc")->string == "2.0" && initialized.Get("id")->number == 1,
          "initialize 必须保留 JSON-RPC 版本和请求 ID");
    Check(initialized.Get("result")->Get("protocolVersion")->string == "2024-11-05" &&
              initialized.Get("result")->Get("capabilities")->Get("tools")->IsObject(),
          "initialize 必须声明 MCP tools 能力");

    const ToolListing listing = ReadAllToolPages(server, "remote-session");
    Check(listing.response_sizes.size() > 1 &&
              listing.names ==
                  std::vector<std::string>{"schedule.create", "schedule.query", "schedule.update", "schedule.delete"},
          "tools/list 必须按 cursor 分页完整、稳定地返回一次性日程工具");
    const auto* create_schema = listing.first_tool.Get("inputSchema");
    Check(create_schema->Get("required")->array.size() == 1 &&
              create_schema->Get("required")->array[0].string == "event" &&
              create_schema->Get("properties")->Get("start_time")->Get("type")->string == "string" &&
              create_schema->Get("properties")->Get("start_time")->Get("default") == nullptr,
          "可选日程时间不得被伪造成带默认值的必填参数");

    voicelife::runtime::LinxMcpToolOutcome call_outcome;
    const auto call = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":"创建会议","start_time":"2030-03-18 00:00:00"}},"id":3})",
        server, {}, &call_outcome);
    Check(call.ok(), "tools/call 应分发给日程工具并回传文本结果");
    const auto& called = ParseMcpEnvelope(*call.value);
    Check(called.Get("result")->Get("content")->array.size() == 1 &&
              called.Get("result")->Get("content")->array[0].Get("type")->string == "text" &&
              called.Get("result")->Get("content")->array[0].Get("text")->string.find("\"event\":\"创建会议\"") !=
                  std::string::npos,
          "tools/call 必须返回 MCP text content");
    Check(called.Get("result")->Get("isError")->boolean == false, "成功 tools/call 必须明确声明 isError=false");
    Check(call_outcome.success && call_outcome.result_status == "success", "直接执行路径必须保留受控的业务成功状态");
    const auto successful_outcome = voicelife::runtime::InspectLinxMcpToolOutcome(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":"创建会议","start_time":1900000000}},"id":3})",
        call);
    Check(successful_outcome.success && successful_outcome.summary == "日程已创建",
          "成功 MCP 机器结果不得进入用户可见会话/屏幕语义");

    const auto binding_response = voicelife::Result<std::string>::Success(
        R"({"type":"mcp","payload":{"jsonrpc":"2.0","id":6,"result":{"content":[],"isError":false}}})");
    const auto binding_outcome = voicelife::runtime::InspectLinxMcpToolOutcome(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"im.binding.start","arguments":{}},"id":6})",
        binding_response);
    Check(binding_outcome.success && voicelife::runtime::IsBindingMcpToolSummary(binding_outcome.summary),
          "绑定工具结果必须带独立语义，禁止降级成日程操作结果覆盖绑定码页面");

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

    voicelife::runtime::LinxMcpToolOutcome conflict_outcome;
    const auto conflict = voicelife::runtime::HandleLinxMcpPayload(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":"冲突会议","start_time":"2030-03-18 00:00:00"}},"id":7})",
        server, {}, &conflict_outcome);
    Check(conflict.ok() && !conflict_outcome.success && conflict_outcome.result_status == "conflict",
          "冲突是合法 MCP 回包，但运行时必须保留为非成功业务状态");

    const auto schedules_before_unavailable = service.query_schedule({
        .schedule_id = std::nullopt,
        .rule_id = std::nullopt,
        .keyword = std::nullopt,
        .start_from = std::nullopt,
        .start_to = std::nullopt,
        .status = voicelife::schedule::ScheduleStatusFilter::kAll,
        .limit = 10,
        .offset = 0,
    });
    const auto unavailable = voicelife::runtime::BuildLinxMcpUnavailableResponse(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"schedule.create","arguments":{"event":"不应执行"}},"id":"busy-1"})",
        "设备 MCP 正忙，请稍后重试", "remote-session");
    Check(unavailable.ok(), "有界 worker 满载时必须能回传受控 JSON-RPC 错误");
    const auto& unavailable_result = ParseMcpEnvelope(*unavailable.value);
    Check(unavailable_result.Get("id")->string == "busy-1" &&
              unavailable_result.Get("error")->Get("code")->number == -32001,
          "MCP 忙响应必须保留请求 id 并使用稳定的 server-error code");
    const auto schedules_after_unavailable = service.query_schedule({
        .schedule_id = std::nullopt,
        .rule_id = std::nullopt,
        .keyword = std::nullopt,
        .start_from = std::nullopt,
        .start_to = std::nullopt,
        .status = voicelife::schedule::ScheduleStatusFilter::kAll,
        .limit = 10,
        .offset = 0,
    });
    Check(schedules_after_unavailable.result.value.size() == schedules_before_unavailable.result.value.size(),
          "构建 busy 响应不得执行任何日程工具");

    const auto notification_busy = voicelife::runtime::BuildLinxMcpUnavailableResponse(
        R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})", "ignored", "remote-session");
    Check(notification_busy.ok() && notification_busy.value->empty(), "通知在 MCP worker 不可用时也不得错误回包");
    return 0;
}
