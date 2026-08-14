// #235 绑定呈现：独立 OLED/TTS 文案、终态提示与脱敏边界。

#include <string>

#include "im_binding_presentation.h"
#include "support/test_support.h"

using voicelife::im::BindingResult;
using voicelife::im::BindingState;
using voicelife::runtime::BindingPresentation;
using voicelife::runtime::IsCurrentBindingResult;
using voicelife::runtime::PresentBindingResult;
using voicelife::test::Check;

namespace {

BindingResult Result(BindingState state, std::string code = {}, int expiry_minutes = 0) {
    return {.state = state,
            .display_code = std::move(code),
            .expires_at = "2026-08-03T00:10:00.000Z",
            .expires_in_minutes = expiry_minutes};
}

void TestPendingShowsAndSpeaksTheSameCodeOnce() {
    const BindingPresentation presentation = PresentBindingResult(Result(BindingState::kPending, "123456", 10));
    Check(presentation.keep_visible && presentation.announce && presentation.status_text == "10分钟内有效" &&
              presentation.content_text == "绑定 123456" &&
              presentation.speech_text == "请在微信公众号发送：绑定 123456",
          "pending 必须在 OLED 与 TTS 中使用同一六位码，并明确有效期");
}

void TestAlreadyActiveKeepsTheCodeWithoutRepeatingSpeech() {
    const BindingPresentation presentation = PresentBindingResult(Result(BindingState::kAlreadyActive, "123456", 5));
    Check(presentation.keep_visible && !presentation.announce && presentation.status_text == "5分钟内有效" &&
              presentation.content_text == "绑定 123456" && presentation.speech_text.empty(),
          "重复命令应恢复当前绑定码显示，但不得重复播报");
}

void TestTerminalStatesPromptTheUser() {
    const BindingPresentation confirmed = PresentBindingResult(Result(BindingState::kConfirmed));
    Check(!confirmed.keep_visible && confirmed.announce && confirmed.content_text == "绑定成功" &&
              confirmed.speech_text == "微信公众号绑定成功",
          "confirmed 必须显示并播报成功");

    for (const BindingState state : {BindingState::kExpired, BindingState::kCancelled, BindingState::kTimedOut}) {
        const BindingPresentation presentation = PresentBindingResult(Result(state));
        Check(!presentation.keep_visible && presentation.announce && presentation.content_text == "请重新绑定" &&
                  presentation.speech_text == "绑定已过期，请重新获取绑定码",
              "过期、取消和本地截止都必须提示用户重新绑定");
    }
}

void TestPollingStatesDoNotLeakOrSpamTheDisplay() {
    for (const BindingState state : {BindingState::kWaiting, BindingState::kRetrying, BindingState::kIdle,
                                     BindingState::kUnavailable, BindingState::kFailed}) {
        const BindingPresentation presentation = PresentBindingResult(Result(state));
        Check(!presentation.keep_visible && !presentation.announce && presentation.status_text.empty() &&
                  presentation.content_text.empty() && presentation.speech_text.empty(),
              "轮询中间态与内部失败不得刷新屏幕、重复播报或透传内部详情");
    }
}

void TestStaleRuntimeResultsAreRejectedBeforePresentation() {
    const BindingResult old_result = Result(BindingState::kConfirmed);
    Check(!IsCurrentBindingResult(old_result, old_result.generation + 1),
          "重绑后的 Runtime 不得呈现旧会话的 confirmed 结果");
    Check(IsCurrentBindingResult(old_result, old_result.generation), "同代次结果必须可被呈现");
}

}  // namespace

int main() {
    TestPendingShowsAndSpeaksTheSameCodeOnce();
    TestAlreadyActiveKeepsTheCodeWithoutRepeatingSpeech();
    TestTerminalStatesPromptTheUser();
    TestPollingStatesDoNotLeakOrSpamTheDisplay();
    TestStaleRuntimeResultsAreRejectedBeforePresentation();
    return 0;
}
