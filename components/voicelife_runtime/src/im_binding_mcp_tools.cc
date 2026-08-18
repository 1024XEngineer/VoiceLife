#include "im_binding_mcp_tools.h"

#include <cstdint>
#include <string>

#include "voicelife/im/im_binding_use_case.h"
#include "voicelife/mcp/mcp_server.h"

namespace voicelife::runtime {
const char* BindingReasonCode(im::BindingState state) {
    switch (state) {
        case im::BindingState::kPending:
            return "created";
        case im::BindingState::kAlreadyActive:
            return "session_active";
        case im::BindingState::kUnavailable:
            return "not_ready";
        case im::BindingState::kWaiting:
            return "waiting_confirmation";
        case im::BindingState::kRetrying:
            return "network_retrying";
        case im::BindingState::kConfirmed:
            return "confirmed";
        case im::BindingState::kExpired:
            return "expired";
        case im::BindingState::kCancelled:
            return "cancelled";
        case im::BindingState::kNotFound:
            return "session_not_found";
        case im::BindingState::kTimedOut:
            return "timed_out";
        case im::BindingState::kCredentialRejected:
            return "credential_rejected";
        case im::BindingState::kIdle:
        case im::BindingState::kFailed:
            return "failed";
    }
    return "failed";
}

/// 是否值得在收到该结果后稍后重试（区别于需要人工换凭据等不可重试场景）。
bool BindingRetryable(im::BindingState state) {
    switch (state) {
        case im::BindingState::kUnavailable:
        case im::BindingState::kRetrying:
        case im::BindingState::kTimedOut:
        case im::BindingState::kExpired:
        case im::BindingState::kNotFound:
        case im::BindingState::kCancelled:
            return true;
        default:
            return false;
    }
}

std::string BindingMessage(im::BindingState state) {
    switch (state) {
        case im::BindingState::kPending:
            return "绑定码已生成，请在公众号输入";
        case im::BindingState::kAlreadyActive:
            return "绑定正在进行中，请使用当前绑定码";
        case im::BindingState::kUnavailable:
            return "绑定功能暂不可用，请稍后再试";
        case im::BindingState::kCredentialRejected:
            return "设备凭据无效，无法完成绑定";
        case im::BindingState::kTimedOut:
        case im::BindingState::kExpired:
            return "绑定窗口已过期，请重新绑定";
        case im::BindingState::kCancelled:
            return "绑定已取消，请重新绑定";
        case im::BindingState::kNotFound:
            return "绑定会话不存在，请重新绑定";
        case im::BindingState::kRetrying:
            return "绑定网络暂时不稳定，正在重试";
        case im::BindingState::kConfirmed:
            return "微信公众号绑定成功";
        case im::BindingState::kIdle:
            return "绑定尚未开始";
        case im::BindingState::kWaiting:
            return "绑定正在等待确认";
        case im::BindingState::kFailed:
            return "绑定失败，请稍后再试";
    }
    return "绑定失败，请稍后再试";
}

const char* BindingStatusName(im::BindingState state) {
    switch (state) {
        case im::BindingState::kIdle:
            return "idle";
        case im::BindingState::kUnavailable:
            return "unavailable";
        case im::BindingState::kPending:
            return "pending";
        case im::BindingState::kWaiting:
            return "waiting";
        case im::BindingState::kRetrying:
            return "retrying";
        case im::BindingState::kAlreadyActive:
            return "already_active";
        case im::BindingState::kConfirmed:
            return "confirmed";
        case im::BindingState::kExpired:
            return "expired";
        case im::BindingState::kCancelled:
            return "cancelled";
        case im::BindingState::kNotFound:
            return "not_found";
        case im::BindingState::kTimedOut:
            return "timed_out";
        case im::BindingState::kCredentialRejected:
            return "credential_rejected";
        case im::BindingState::kFailed:
            return "failed";
    }
    return "failed";
}

Status RegisterImBindingMcpTools(mcp::McpServer& server, im::BindingUseCase& use_case, BindingResultHook on_result) {
    return server.add_tool(
        "im.binding.start",
        "创建 IM 平台绑定会话并返回六位绑定码；用户须在公众号发送「绑定 <六位码>」完成设备绑定，例如：绑定 123456。",
        mcp::PropertyList({mcp::Property::WithIntegerRange("expires_in_minutes", 1, 10, int64_t{10})}),
        [&use_case, on_result = std::move(on_result)](const mcp::PropertyList& properties) {
            // 越界参数已被 MCP 边界按 Schema（1～10）拒绝；此处 int64→int 转换安全。
            const int expires_in_minutes =
                static_cast<int>(properties.value<int64_t>("expires_in_minutes").value_or(10));
            const im::BindingResult result = use_case.Start(expires_in_minutes);
            // 每次语音命令都把脱敏结果交给 Runtime：already_active 可以恢复被普通
            // 对话覆盖的绑定码，创建失败也必须在设备侧给出确定反馈。
            if (on_result) on_result(result);
            ToolOutputObject fields{
                MakeToolOutput("status", ToolOutputValue::String(BindingStatusName(result.state))),
                MakeToolOutput("reason", ToolOutputValue::String(BindingReasonCode(result.state))),
                MakeToolOutput("retryable", ToolOutputValue::String(BindingRetryable(result.state) ? "true" : "false")),
                MakeToolOutput("message", ToolOutputValue::String(BindingMessage(result.state))),
            };
            if (!result.display_code.empty()) {
                fields.emplace_back(MakeToolOutput("display_code", ToolOutputValue::String(result.display_code)));
                if (result.state == im::BindingState::kPending) {
                    // 确定性播报指令：直接给出「绑定 + 六位码」完整句子，不让模型临场发挥。
                    fields.emplace_back(MakeToolOutput(
                        "speak_text", ToolOutputValue::String("请在微信公众号发送：绑定 " + result.display_code)));
                }
            }
            if (!result.expires_at.empty()) {
                fields.emplace_back(MakeToolOutput("expires_at", ToolOutputValue::String(result.expires_at)));
            }
            return ToolResult{.status = Status::Ok(), .output = ToolOutputValue::Object(std::move(fields))};
        });
}

}  // namespace voicelife::runtime
