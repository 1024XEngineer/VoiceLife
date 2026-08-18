#include "voicelife/application/interaction_orchestrator.h"

#include <string_view>
#include <utility>
#include <vector>

#include "support/test_support.h"

namespace {

using voicelife::application::InteractionAction;
using voicelife::application::InteractionActionSink;
using voicelife::application::InteractionEvent;
using voicelife::application::InteractionOrchestrator;
using voicelife::test::Check;
using voicelife::voice::VoiceInteractionAction;
using voicelife::voice::VoiceInteractionEvent;
using voicelife::voice::VoiceInteractionState;

class TraceSink final : public InteractionActionSink {
   public:
    voicelife::Status Submit(InteractionAction action) override {
        trace.push_back(action);
        return voicelife::Status::Ok();
    }

    std::vector<InteractionAction> trace;
};

class RejectingSink final : public InteractionActionSink {
   public:
    voicelife::Status Submit(InteractionAction action) override {
        submitted = std::move(action);
        return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "动作投影不可用");
    }

    InteractionAction submitted;
};

void Submit(InteractionOrchestrator& orchestrator, TraceSink& trace, VoiceInteractionEvent event,
            std::string_view wake_word = {}) {
    const voicelife::Status result = orchestrator.Handle({.voice_event = event, .wake_word = wake_word}, trace);
    Check(result.ok(), "合法交互事件必须被应用层接受");
}

}  // namespace

int main() {
    InteractionOrchestrator orchestrator;
    TraceSink trace;
    const std::vector<InteractionEvent> events = {
        {.voice_event = VoiceInteractionEvent::kBootCompleted, .wake_word = {}},
        {.voice_event = VoiceInteractionEvent::kWakeDetected, .wake_word = "hello"},
        {.voice_event = VoiceInteractionEvent::kInterruptAndAcknowledge, .wake_word = "stop"},
        {.voice_event = VoiceInteractionEvent::kEndpointDetected, .wake_word = {}},
        {.voice_event = VoiceInteractionEvent::kFinalizationTimedOut, .wake_word = {}},
    };
    const std::vector<InteractionAction> expected_trace = {
        {.source = VoiceInteractionEvent::kBootCompleted,
         .state = VoiceInteractionState::kStandby,
         .directive = VoiceInteractionAction::kRestoreStandby,
         .wake_word = {}},
        {.source = VoiceInteractionEvent::kWakeDetected,
         .state = VoiceInteractionState::kListening,
         .directive = VoiceInteractionAction::kStartVoiceTurn,
         .wake_word = "hello"},
        {.source = VoiceInteractionEvent::kInterruptAndAcknowledge,
         .state = VoiceInteractionState::kListening,
         .directive = VoiceInteractionAction::kInterruptAndStartVoiceTurn,
         .wake_word = "stop"},
        {.source = VoiceInteractionEvent::kEndpointDetected,
         .state = VoiceInteractionState::kFinalizing,
         .directive = VoiceInteractionAction::kStopVoiceTurn,
         .wake_word = {}},
        {.source = VoiceInteractionEvent::kFinalizationTimedOut,
         .state = VoiceInteractionState::kStandby,
         .directive = VoiceInteractionAction::kRestoreStandby,
         .wake_word = {}},
    };

    for (const InteractionEvent event : events) {
        Submit(orchestrator, trace, event.voice_event, event.wake_word);
    }

    Check(trace.trace == expected_trace, "引导、唤醒、打断、超时必须保留状态和动作轨迹");
    Check(trace.trace[1].wake_word == "hello" && trace.trace[2].wake_word == "stop",
          "唤醒与打断的动作轨迹必须保留各自的关键参数");
    Check(orchestrator.state() == VoiceInteractionState::kStandby, "最终 STT 超时后必须恢复待机");

    const InteractionAction baseline = trace.trace[1];
    Check(!(baseline == InteractionAction{.source = VoiceInteractionEvent::kBootCompleted,
                                          .state = baseline.state,
                                          .directive = baseline.directive,
                                          .wake_word = baseline.wake_word}),
          "动作比较必须包含来源事件");
    Check(!(baseline == InteractionAction{.source = baseline.source,
                                          .state = VoiceInteractionState::kStandby,
                                          .directive = baseline.directive,
                                          .wake_word = baseline.wake_word}),
          "动作比较必须包含迁移后的状态");
    Check(!(baseline == InteractionAction{.source = baseline.source,
                                          .state = baseline.state,
                                          .directive = VoiceInteractionAction::kStopVoiceTurn,
                                          .wake_word = baseline.wake_word}),
          "动作比较必须包含执行指令");
    Check(!(baseline == InteractionAction{.source = baseline.source,
                                          .state = baseline.state,
                                          .directive = baseline.directive,
                                          .wake_word = "different"}),
          "动作比较必须包含唤醒参数");

    InteractionOrchestrator rejecting_orchestrator;
    RejectingSink rejecting_sink;
    const voicelife::Status projection_failure = rejecting_orchestrator.Handle(
        {.voice_event = VoiceInteractionEvent::kBootCompleted, .wake_word = {}}, rejecting_sink);
    Check(!projection_failure.ok() && projection_failure.code == voicelife::ErrorCode::kUnavailable,
          "动作端口失败必须透传给调用方");
    Check(rejecting_sink.submitted == InteractionAction{.source = VoiceInteractionEvent::kBootCompleted,
                                                        .state = VoiceInteractionState::kStandby,
                                                        .directive = VoiceInteractionAction::kRestoreStandby,
                                                        .wake_word = {}},
          "动作端口失败前仍须收到完整的迁移动作");
    Check(rejecting_orchestrator.state() == VoiceInteractionState::kStandby, "动作端口失败不得回滚既有状态机迁移语义");

    InteractionOrchestrator tts_orchestrator;
    TraceSink tts_trace;
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kBootCompleted);
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kWakeDetected);
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kIntentReceived);
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kTtsStarted);
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kTtsStopped);
    Check(tts_trace.trace.back() == InteractionAction{.source = VoiceInteractionEvent::kTtsStopped,
                                                      .state = VoiceInteractionState::kListening,
                                                      .directive = VoiceInteractionAction::kStartCapture,
                                                      .wake_word = {}},
          "TTS 结束后必须恢复 follow-up 聆听");

    const voicelife::Status rejected =
        orchestrator.Handle({.voice_event = VoiceInteractionEvent::kTtsStopped, .wake_word = {}}, trace);
    Check(!rejected.ok(), "乱序事件必须被拒绝且不产生动作");
    return 0;
}
