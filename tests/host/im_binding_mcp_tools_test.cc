// #235 im.binding.start MCP 工具：参数、返回契约与业务错误可播报映射（TDD RED）。

#include "im_binding_mcp_tools.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "support/im_pairing_test_support.h"
#include "support/test_support.h"
#include "voicelife/im/im_binding_use_case.h"
#include "voicelife/mcp/mcp_server.h"

using voicelife::ErrorCode;
using voicelife::ToolCall;
using voicelife::im::BindingUseCase;
using voicelife::im::ImPairingClock;
using voicelife::im::PairingClientStatus;
using voicelife::mcp::McpServer;
using voicelife::test::Check;

namespace {

class FakeClock final : public ImPairingClock {
   public:
    uint64_t now_ms = 1000;
    uint64_t unix_ms = 1785715200000ULL;
    uint64_t MonotonicMillis() const override { return now_ms; }
    uint64_t UnixMillis() const override { return unix_ms; }
};

void Prepare(FakePairingPort& port) {
    port.created = {.status = PairingClientStatus::kSuccess,
                    .value = CreatedSession("2026-08-03T00:00:00.000Z", "2026-08-03T00:05:00.000Z"),
                    .message = {}};
}

void TestRegistersAndCreatesBinding() {
    FakePairingPort port;
    FakeClock clock;
    Prepare(port);
    BindingUseCase use_case(port, clock);
    use_case.set_user_id("user-fixture");
    McpServer server;
    Check(voicelife::runtime::RegisterImBindingMcpTools(server, use_case).ok(), "绑定 MCP 工具应注册成功");

    const auto listed = server.list_tools();
    const bool found = std::any_of(listed.tools.begin(), listed.tools.end(),
                                   [](const auto& tool) { return tool.name == "im.binding.start"; });
    Check(found, "tools/list 必须公开 im.binding.start");

    const auto result = server.call({.request_id = "bind-1", .name = "im.binding.start", .arguments = {}});
    Check(result.status.ok() && result.output.at("status") == "pending" &&
              result.output.at("display_code") == "123456" && result.output.contains("expires_at") &&
              !result.output.at("message").empty(),
          "无参调用应使用十分钟默认值并返回可播报绑定码信息");

    const auto duplicate = server.call({.request_id = "bind-2", .name = "im.binding.start", .arguments = {}});
    Check(duplicate.status.ok() && duplicate.output.at("status") == "already_active" &&
              !duplicate.output.at("message").empty(),
          "重复语音命令应返回可播报 already_active，而非创建无界会话");
}

void TestAcceptsExplicitExpiryAndRejectsInvalidArguments() {
    FakePairingPort port;
    FakeClock clock;
    Prepare(port);
    BindingUseCase use_case(port, clock);
    use_case.set_user_id("user-fixture");
    McpServer server;
    Check(voicelife::runtime::RegisterImBindingMcpTools(server, use_case).ok(), "绑定工具应可注册");

    const auto explicit_expiry = server.call(
        {.request_id = "bind-3", .name = "im.binding.start", .arguments = {{"expires_in_minutes", int64_t{5}}}});
    Check(explicit_expiry.status.ok() && explicit_expiry.output.at("status") == "pending",
          "显式有效期应通过工具参数契约");

    McpServer invalid_server;
    BindingUseCase invalid_use_case;
    Check(voicelife::runtime::RegisterImBindingMcpTools(invalid_server, invalid_use_case).ok(), "绑定工具应可注册");
    const auto wrong_type = invalid_server.call({.request_id = "bind-4",
                                                 .name = "im.binding.start",
                                                 .arguments = {{"expires_in_minutes", std::string("ten")}}});
    Check(wrong_type.status.code == ErrorCode::kInvalidArgument, "错误参数类型应由 MCP 边界拒绝");
    const auto unknown = invalid_server.call(
        {.request_id = "bind-5", .name = "im.binding.start", .arguments = {{"unknown", int64_t{1}}}});
    Check(unknown.status.code == ErrorCode::kInvalidArgument, "未知参数应由 MCP 边界拒绝");
}

void TestReturnsSpeakableUnavailableResult() {
    BindingUseCase use_case;
    McpServer server;
    Check(voicelife::runtime::RegisterImBindingMcpTools(server, use_case).ok(), "绑定工具应可注册");
    const auto result = server.call({.request_id = "bind-6", .name = "im.binding.start", .arguments = {}});
    Check(result.status.ok() && result.output.at("status") == "unavailable" && !result.output.at("message").empty() &&
              !result.output.contains("display_code"),
          "IM 未 ready 时应返回可播报 unavailable，而非 JSON-RPC error");
}

}  // namespace

int main() {
    TestRegistersAndCreatesBinding();
    TestAcceptsExplicitExpiryAndRejectsInvalidArguments();
    TestReturnsSpeakableUnavailableResult();
    return 0;
}
