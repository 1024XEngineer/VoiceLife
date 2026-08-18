#include "voicelife/application/interaction_orchestrator.h"

#include <string_view>
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
        {.voice_event = VoiceInteractionEvent::kBootCompleted},
        {.voice_event = VoiceInteractionEvent::kWakeDetected, .wake_word = "hello"},
        {.voice_event = VoiceInteractionEvent::kInterruptAndAcknowledge, .wake_word = "stop"},
        {.voice_event = VoiceInteractionEvent::kEndpointDetected},
        {.voice_event = VoiceInteractionEvent::kFinalizationTimedOut},
    };
    const std::vector<InteractionAction> expected_trace = {
        {.source = VoiceInteractionEvent::kBootCompleted,
         .state = VoiceInteractionState::kStandby,
         .directive = VoiceInteractionAction::kRestoreStandby},
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
         .directive = VoiceInteractionAction::kStopVoiceTurn},
        {.source = VoiceInteractionEvent::kFinalizationTimedOut,
         .state = VoiceInteractionState::kStandby,
         .directive = VoiceInteractionAction::kRestoreStandby},
    };

    for (const InteractionEvent event : events) {
        Submit(orchestrator, trace, event.voice_event, event.wake_word);
    }

    Check(trace.trace == expected_trace, "引导、唤醒、打断、超时必须保留状态和动作轨迹");
    Check(trace.trace[1].wake_word == "hello" && trace.trace[2].wake_word == "stop",
          "唤醒与打断的动作轨迹必须保留各自的关键参数");
    Check(orchestrator.state() == VoiceInteractionState::kStandby, "最终 STT 超时后必须恢复待机");

    InteractionOrchestrator tts_orchestrator;
    TraceSink tts_trace;
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kBootCompleted);
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kWakeDetected);
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kIntentReceived);
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kTtsStarted);
    Submit(tts_orchestrator, tts_trace, VoiceInteractionEvent::kTtsStopped);
    Check(tts_trace.trace.back() == InteractionAction{.source = VoiceInteractionEvent::kTtsStopped,
                                                      .state = VoiceInteractionState::kListening,
                                                      .directive = VoiceInteractionAction::kStartCapture},
          "TTS 结束后必须恢复 follow-up 聆听");

    const voicelife::Status rejected = orchestrator.Handle({.voice_event = VoiceInteractionEvent::kTtsStopped}, trace);
    Check(!rejected.ok(), "乱序事件必须被拒绝且不产生动作");
    return 0;
}
