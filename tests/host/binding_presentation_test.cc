// #235 绑定呈现：独立 OLED/TTS 文案、终态提示与脱敏边界。

#include <string>

#include "im_binding_presentation.h"
#include "support/test_support.h"

using voicelife::im::BindingResult;
using voicelife::im::BindingState;
using voicelife::runtime::BindingPresentation;
using voicelife::runtime::IsCurrentBindingResult;
using voicelife::runtime::kBindingSystemSpeechCapacity;
using voicelife::runtime::PresentBindingResult;
using voicelife::runtime::ShouldEndVoiceTurnAfterBindingResult;
using voicelife::test::Check;

namespace {

BindingResult Result(BindingState state, std::string code = {}, int expiry_minutes = 0) {
    return {.state = state,
            .display_code = std::move(code),
            .expires_at = "2026-08-03T00:10:00.000Z",
            .expires_in_minutes = expiry_minutes,
            .message = {}};
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

    const BindingPresentation expired = PresentBindingResult(Result(BindingState::kExpired));
    Check(!expired.keep_visible && expired.announce && expired.content_text == "绑定已过期" &&
              expired.speech_text == "绑定已过期，请重新获取绑定码",
          "expired 必须提示用户重新获取绑定码");

    const BindingPresentation cancelled = PresentBindingResult(Result(BindingState::kCancelled));
    Check(!cancelled.keep_visible && cancelled.announce && cancelled.content_text == "绑定已取消" &&
              cancelled.speech_text == "绑定已取消，请重新获取绑定码",
          "cancelled 不得伪装成自然过期");

    const BindingPresentation timed_out = PresentBindingResult(Result(BindingState::kTimedOut));
    Check(!timed_out.keep_visible && timed_out.announce && timed_out.content_text == "等待超时" &&
              timed_out.speech_text == "等待确认超时，请重新获取绑定码",
          "timed_out 必须明确是本地等待截止");
}

void TestFailureStatesGiveSafeDeviceFeedback() {
    for (const BindingState state : {BindingState::kUnavailable, BindingState::kFailed,
                                     BindingState::kCredentialRejected, BindingState::kNotFound}) {
        const BindingPresentation presentation = PresentBindingResult(Result(state));
        Check(!presentation.keep_visible && presentation.announce && !presentation.status_text.empty() &&
                  !presentation.content_text.empty() && !presentation.speech_text.empty(),
              "创建或轮询失败必须有脱敏的 OLED/TTS 反馈，不能只留在 MCP 返回中");
    }
}

void TestPollingStatesDoNotLeakOrSpamTheDisplay() {
    for (const BindingState state : {BindingState::kWaiting, BindingState::kRetrying, BindingState::kIdle}) {
        const BindingPresentation presentation = PresentBindingResult(Result(state));
        Check(!presentation.keep_visible && !presentation.announce && presentation.status_text.empty() &&
                  presentation.content_text.empty() && presentation.speech_text.empty(),
              "轮询中间态不得刷新屏幕、重复播报或透传内部详情");
    }
}

void TestStaleRuntimeResultsAreRejectedBeforePresentation() {
    const BindingResult old_result = Result(BindingState::kConfirmed);
    Check(!IsCurrentBindingResult(old_result, old_result.generation + 1),
          "重绑后的 Runtime 不得呈现旧会话的 confirmed 结果");
    Check(IsCurrentBindingResult(old_result, old_result.generation), "同代次结果必须可被呈现");
}

void TestBindingCodeEndsOnlyItsActiveVoiceTurn() {
    Check(ShouldEndVoiceTurnAfterBindingResult(Result(BindingState::kPending, "123456", 10), true),
          "活跃语音回合生成绑定码后，播报结束必须直接回待机");
    Check(ShouldEndVoiceTurnAfterBindingResult(Result(BindingState::kAlreadyActive, "123456", 10), true),
          "活跃语音回合恢复现有绑定码后也不得进入 follow-up 聆听");
    Check(!ShouldEndVoiceTurnAfterBindingResult(Result(BindingState::kPending, "123456", 10), false),
          "待机期间的绑定结果不得伪造语音回合收尾");
    Check(!ShouldEndVoiceTurnAfterBindingResult(Result(BindingState::kConfirmed), true),
          "后台确认结果不得错误终止用户正在进行的其他语音回合");
}

void TestDeviceBindingSpeechNeverRequiresTruncation() {
    for (const BindingState state :
         {BindingState::kPending, BindingState::kConfirmed, BindingState::kExpired, BindingState::kCancelled,
          BindingState::kTimedOut, BindingState::kUnavailable, BindingState::kCredentialRejected,
          BindingState::kNotFound, BindingState::kFailed}) {
        const BindingPresentation presentation =
            PresentBindingResult(Result(state, state == BindingState::kPending ? "123456" : std::string{}, 10));
        Check(presentation.speech_text.size() < kBindingSystemSpeechCapacity,
              "所有固定绑定 TTS 文案必须完整装入 BoardRequest，禁止静默截断");
    }
}

}  // namespace

int main() {
    TestPendingShowsAndSpeaksTheSameCodeOnce();
    TestAlreadyActiveKeepsTheCodeWithoutRepeatingSpeech();
    TestTerminalStatesPromptTheUser();
    TestFailureStatesGiveSafeDeviceFeedback();
    TestPollingStatesDoNotLeakOrSpamTheDisplay();
    TestStaleRuntimeResultsAreRejectedBeforePresentation();
    TestBindingCodeEndsOnlyItsActiveVoiceTurn();
    TestDeviceBindingSpeechNeverRequiresTruncation();
    return 0;
}
