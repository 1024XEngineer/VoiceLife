// #235 im.binding.start MCP 工具：参数范围契约、already_active 携带当前码、
// 会话开始 hook 与 retryable/reason/speak_text 输出字段。

#include "im_binding_mcp_tools.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <optional>
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

/**
 * @brief 从工具输出对象中读取指定字符串字段。
 * @param output 工具输出值。
 * @param key 字段名。
 * @return 字段存在且为字符串时返回其值。
 */
std::optional<std::string> OutputString(const voicelife::ToolOutputValue& output, const std::string& key) {
    if (!output.IsObject() || output.object == nullptr) return std::nullopt;
    for (const auto& [name, value] : *output.object) {
        if (name == key && value != nullptr && value->IsString()) return value->string;
    }
    return std::nullopt;
}

/**
 * @brief 判断工具输出对象中是否存在指定字段。
 * @param output 工具输出值。
 * @param key 字段名。
 * @return 存在字符串字段时返回 true。
 */
bool OutputContains(const voicelife::ToolOutputValue& output, const std::string& key) {
    return OutputString(output, key).has_value();
}

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
    Check(result.status.ok() && OutputString(result.output, "status") == "pending" &&
              OutputString(result.output, "display_code") == "123456" && OutputContains(result.output, "expires_at") &&
              !OutputString(result.output, "message")->empty() && OutputString(result.output, "reason") == "created" &&
              OutputString(result.output, "retryable") == "false" &&
              OutputString(result.output, "speak_text") == "请在微信公众号发送：绑定 123456",
          "无参调用应使用十分钟默认值并返回可播报绑定码信息、speak_text 与稳定字段");

    const auto duplicate = server.call({.request_id = "bind-2", .name = "im.binding.start", .arguments = {}});
    Check(duplicate.status.ok() && OutputString(duplicate.output, "status") == "already_active" &&
              OutputString(duplicate.output, "display_code") == "123456" &&
              !OutputString(duplicate.output, "message")->empty(),
          "重复语音命令应返回携带当前码的 already_active，而非创建无界会话");
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
    Check(explicit_expiry.status.ok() && OutputString(explicit_expiry.output, "status") == "pending",
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

void TestRejectsOutOfRangeExpiryAtBoundary() {
    FakePairingPort port;
    FakeClock clock;
    Prepare(port);
    BindingUseCase use_case(port, clock);
    use_case.set_user_id("user-fixture");
    McpServer server;
    Check(voicelife::runtime::RegisterImBindingMcpTools(server, use_case).ok(), "绑定工具应可注册");

    for (const int64_t invalid :
         {int64_t{0}, int64_t{-1}, int64_t{11}, int64_t{100}, int64_t{INT64_MAX}, int64_t{INT32_MAX} + 1}) {
        const auto result = server.call(
            {.request_id = "bind-range", .name = "im.binding.start", .arguments = {{"expires_in_minutes", invalid}}});
        Check(result.status.code == ErrorCode::kInvalidArgument,
              "越界有效期（含 INT64_MAX 截断场景）必须由 MCP 边界以 kInvalidArgument 拒绝，不得静默改值");
    }
}

void TestInvokesResultHookAndCarriesFields() {
    FakePairingPort port;
    FakeClock clock;
    Prepare(port);
    BindingUseCase use_case(port, clock);
    use_case.set_user_id("user-fixture");
    McpServer server;
    int hook_count = 0;
    voicelife::im::BindingResult hook_result;
    Check(voicelife::runtime::RegisterImBindingMcpTools(
              server, use_case,
              [&hook_count, &hook_result](const voicelife::im::BindingResult& result) {
                  ++hook_count;
                  hook_result = result;
              })
              .ok(),
          "带 hook 的绑定工具应可注册");

    const auto first = server.call({.request_id = "bind-hook-1", .name = "im.binding.start", .arguments = {}});
    Check(first.status.ok() && first.output.at("status") == "pending" && hook_count == 1 &&
              hook_result.state == voicelife::im::BindingState::kPending && hook_result.display_code == "123456" &&
              hook_result.generation != 0,
          "创建成功必须恰好触发一次并携带脱敏结果与代次的会话开始 hook");
    const auto second = server.call({.request_id = "bind-hook-2", .name = "im.binding.start", .arguments = {}});
    Check(second.status.ok() && second.output.at("status") == "already_active" &&
              second.output.at("display_code") == "123456" && second.output.at("reason") == "session_active" &&
              second.output.at("retryable") == "false" && hook_count == 2 &&
              hook_result.state == voicelife::im::BindingState::kAlreadyActive,
          "already_active 必须投递当前码，以恢复被普通语音覆盖的 OLED 内容，但不重启轮询");
}

void TestReturnsSpeakableUnavailableResult() {
    BindingUseCase use_case;
    McpServer server;
    int hook_count = 0;
    voicelife::im::BindingResult hook_result;
    Check(voicelife::runtime::RegisterImBindingMcpTools(
              server, use_case,
              [&hook_count, &hook_result](const voicelife::im::BindingResult& result) {
                  ++hook_count;
                  hook_result = result;
              })
              .ok(),
          "绑定工具应可注册");
    const auto result = server.call({.request_id = "bind-6", .name = "im.binding.start", .arguments = {}});
    Check(result.status.ok() && result.output.at("status") == "unavailable" && !result.output.at("message").empty() &&
              result.output.at("reason") == "not_ready" && result.output.at("retryable") == "true" &&
              !result.output.contains("display_code") && hook_count == 1 &&
              hook_result.state == voicelife::im::BindingState::kUnavailable,
          "IM 未 ready 时必须投递可呈现 unavailable，而非只返回 MCP 文本");
}

}  // namespace

int main() {
    TestRegistersAndCreatesBinding();
    TestAcceptsExplicitExpiryAndRejectsInvalidArguments();
    TestRejectsOutOfRangeExpiryAtBoundary();
    TestInvokesResultHookAndCarriesFields();
    TestReturnsSpeakableUnavailableResult();
    return 0;
}
