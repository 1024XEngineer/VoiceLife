#include "im_binding_mcp_tools.h"

#include <cstdint>
#include <string>

#include "voicelife/im/im_binding_use_case.h"
#include "voicelife/mcp/mcp_server.h"

namespace voicelife::runtime {
namespace {

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

}  // namespace

Status RegisterImBindingMcpTools(mcp::McpServer& server, im::BindingUseCase& use_case) {
    return server.add_tool(
        "im.binding.start", "创建微信公众号绑定会话并返回六位绑定码；用户在公众号输入该码后完成设备绑定。",
        mcp::PropertyList({mcp::Property("expires_in_minutes", mcp::PropertyType::kInteger, int64_t{10})}),
        [&use_case](const mcp::PropertyList& properties) {
            const int64_t expires = properties.value<int64_t>("expires_in_minutes").value_or(10);
            const im::BindingResult result = use_case.Start(static_cast<int>(expires));
            ToolResult output{.status = Status::Ok(), .output = {}};
            output.output["status"] = BindingStatusName(result.state);
            output.output["message"] = BindingMessage(result.state);
            if (!result.display_code.empty()) output.output["display_code"] = result.display_code;
            if (!result.expires_at.empty()) output.output["expires_at"] = result.expires_at;
            return output;
        });
}

}  // namespace voicelife::runtime
