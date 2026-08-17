#include "im_binding_presentation.h"

#include <string>

namespace voicelife::runtime {
namespace {

std::string ExpiryText(int minutes) { return minutes > 0 ? std::to_string(minutes) + "分钟内有效" : "请尽快完成"; }

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
            return {.keep_visible = false,
                    .announce = true,
                    .status_text = "公众号绑定",
                    .content_text = "绑定成功",
                    .speech_text = "微信公众号绑定成功"};
        case im::BindingState::kExpired:
            return {.keep_visible = false,
                    .announce = true,
                    .status_text = "公众号绑定",
                    .content_text = "绑定已过期",
                    .speech_text = "绑定已过期，请重新获取绑定码"};
        case im::BindingState::kCancelled:
            return {.keep_visible = false,
                    .announce = true,
                    .status_text = "公众号绑定",
                    .content_text = "绑定已取消",
                    .speech_text = "绑定已取消，请重新获取绑定码"};
        case im::BindingState::kTimedOut:
            return {.keep_visible = false,
                    .announce = true,
                    .status_text = "公众号绑定",
                    .content_text = "等待超时",
                    .speech_text = "等待确认超时，请重新获取绑定码"};
        case im::BindingState::kUnavailable:
            return {.keep_visible = false,
                    .announce = true,
                    .status_text = "公众号绑定",
                    .content_text = "暂不可用",
                    .speech_text = "绑定功能暂不可用，请稍后再试"};
        case im::BindingState::kCredentialRejected:
            return {.keep_visible = false,
                    .announce = true,
                    .status_text = "公众号绑定",
                    .content_text = "设备凭据无效",
                    .speech_text = "设备凭据无效，无法完成绑定"};
        case im::BindingState::kNotFound:
            return {.keep_visible = false,
                    .announce = true,
                    .status_text = "公众号绑定",
                    .content_text = "会话不存在",
                    .speech_text = "绑定会话不存在，请重新获取绑定码"};
        case im::BindingState::kFailed:
            return {.keep_visible = false,
                    .announce = true,
                    .status_text = "公众号绑定",
                    .content_text = "绑定失败",
                    .speech_text = "绑定失败，请稍后再试"};
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
