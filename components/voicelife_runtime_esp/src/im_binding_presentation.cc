#include "im_binding_presentation.h"

#include <string>
#include <utility>

namespace voicelife::runtime {
namespace {

std::string ExpiryText(int minutes) { return minutes > 0 ? std::to_string(minutes) + "分钟内有效" : "请尽快完成"; }

BindingPresentation TerminalPresentation(std::string status, std::string content, std::string speech,
                                         bool resume_listening = false) {
    return {.keep_visible = false,
            .announce = true,
            .display_duration_ms = kBindingTerminalDisplayDurationMs,
            .resume_listening = resume_listening,
            .status_text = std::move(status),
            .content_text = std::move(content),
            .speech_text = std::move(speech)};
}

BindingPresentation CodePresentation(const im::BindingResult& result, bool announce) {
    if (result.display_code.empty()) return {};
    BindingPresentation presentation{
        .keep_visible = true,
        .announce = announce,
        .status_text = ExpiryText(result.expires_in_minutes),
        .content_text = "绑定 " + result.display_code,
        .speech_text = {},
    };
    if (announce) presentation.speech_text = "请在微信公众号发送：绑定 " + result.display_code;
    return presentation;
}

}  // namespace

BindingPresentation PresentBindingResult(const im::BindingResult& result) {
    switch (result.state) {
        case im::BindingState::kPending:
            return CodePresentation(result, true);
        case im::BindingState::kAlreadyActive:
            return CodePresentation(result, false);
        case im::BindingState::kConfirmed:
            return TerminalPresentation("公众号绑定", "绑定成功", "微信公众号绑定成功", true);
        case im::BindingState::kExpired:
            return TerminalPresentation("公众号绑定", "绑定已过期", "绑定已过期，请重新获取绑定码");
        case im::BindingState::kCancelled:
            return TerminalPresentation("公众号绑定", "绑定已取消", "绑定已取消，请重新获取绑定码");
        case im::BindingState::kTimedOut:
            return TerminalPresentation("公众号绑定", "等待超时", "等待确认超时，请重新获取绑定码");
        case im::BindingState::kUnavailable:
            return TerminalPresentation("公众号绑定", "暂不可用", "绑定功能暂不可用，请稍后再试");
        case im::BindingState::kCredentialRejected:
            return TerminalPresentation("公众号绑定", "设备凭据无效", "设备凭据无效，无法完成绑定");
        case im::BindingState::kNotFound:
            return TerminalPresentation("公众号绑定", "会话不存在", "绑定会话不存在，请重新获取绑定码");
        case im::BindingState::kFailed:
            return TerminalPresentation("公众号绑定", "绑定失败", "绑定失败，请稍后再试");
        default:
            return {};
    }
}

bool IsCurrentBindingResult(const im::BindingResult& result, uint64_t current_generation) {
    return result.generation == current_generation;
}

bool ShouldEndVoiceTurnAfterBindingResult(const im::BindingResult& result, bool active_voice_turn) {
    return active_voice_turn &&
           (result.state == im::BindingState::kPending || result.state == im::BindingState::kAlreadyActive);
}

}  // namespace voicelife::runtime
