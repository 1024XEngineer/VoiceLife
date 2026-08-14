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
        case im::BindingState::kCancelled:
        case im::BindingState::kTimedOut:
            return {.keep_visible = false,
                    .announce = true,
                    .status_text = "公众号绑定",
                    .content_text = "请重新绑定",
                    .speech_text = "绑定已过期，请重新获取绑定码"};
        default:
            return {};
    }
}

bool IsCurrentBindingResult(const im::BindingResult& result, uint64_t current_generation) {
    return result.generation == current_generation;
}

}  // namespace voicelife::runtime
