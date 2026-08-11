#include "voicelife/voice/voice_interaction_controller.h"

#include <string>

namespace voicelife::voice {
namespace {

Result<VoiceInteractionTransition> InvalidTransition(VoiceInteractionState state, VoiceInteractionEvent event) {
    return Result<VoiceInteractionTransition>::Failure(
        ErrorCode::kConflict, "板端交互状态不接受事件 " + std::to_string(static_cast<int>(event)) + "，当前状态 " +
                                  std::to_string(static_cast<int>(state)));
}

}  // namespace

Result<VoiceInteractionTransition> VoiceInteractionController::Handle(VoiceInteractionEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    VoiceInteractionTransition transition{.state = state_};
    switch (event) {
        case VoiceInteractionEvent::kBootCompleted:
            if (state_ != VoiceInteractionState::kBooting) return InvalidTransition(state_, event);
            state_ = VoiceInteractionState::kStandby;
            transition.action = VoiceInteractionAction::kRestoreStandby;
            break;
        case VoiceInteractionEvent::kToggleChat:
            if (state_ == VoiceInteractionState::kStandby) {
                state_ = VoiceInteractionState::kListening;
                transition.action = VoiceInteractionAction::kStartCapture;
            } else if (state_ == VoiceInteractionState::kListening) {
                state_ = VoiceInteractionState::kStandby;
                transition.action = VoiceInteractionAction::kStopVoiceTurn;
            } else if (state_ == VoiceInteractionState::kSpeaking || state_ == VoiceInteractionState::kThinking) {
                state_ = VoiceInteractionState::kInterrupting;
                transition.action = VoiceInteractionAction::kInterruptSession;
            } else {
                return InvalidTransition(state_, event);
            }
            break;
        case VoiceInteractionEvent::kPressDown:
            if (state_ == VoiceInteractionState::kStandby) {
                state_ = VoiceInteractionState::kListening;
                transition.action = VoiceInteractionAction::kStartCapture;
            } else if (state_ == VoiceInteractionState::kSpeaking || state_ == VoiceInteractionState::kThinking) {
                state_ = VoiceInteractionState::kListening;
                transition.action = VoiceInteractionAction::kInterruptAndStartCapture;
            } else {
                return InvalidTransition(state_, event);
            }
            break;
        case VoiceInteractionEvent::kPressUp:
            if (state_ != VoiceInteractionState::kListening) return InvalidTransition(state_, event);
            state_ = VoiceInteractionState::kStandby;
            transition.action = VoiceInteractionAction::kStopVoiceTurn;
            break;
        case VoiceInteractionEvent::kWakeDetected:
            if (state_ == VoiceInteractionState::kStandby) {
                state_ = VoiceInteractionState::kListening;
                transition.action = VoiceInteractionAction::kStartVoiceTurn;
            } else if (state_ == VoiceInteractionState::kListening) {
                state_ = VoiceInteractionState::kStandby;
                transition.action = VoiceInteractionAction::kStopVoiceTurn;
            } else if (state_ == VoiceInteractionState::kSpeaking || state_ == VoiceInteractionState::kThinking) {
                state_ = VoiceInteractionState::kInterrupting;
                transition.action = VoiceInteractionAction::kInterruptSession;
            } else {
                return InvalidTransition(state_, event);
            }
            break;
        case VoiceInteractionEvent::kCaptureStarted:
            if (state_ != VoiceInteractionState::kListening) return InvalidTransition(state_, event);
            break;
        case VoiceInteractionEvent::kIntentReceived:
            if (state_ != VoiceInteractionState::kListening && state_ != VoiceInteractionState::kThinking) {
                return InvalidTransition(state_, event);
            }
            state_ = VoiceInteractionState::kThinking;
            break;
        case VoiceInteractionEvent::kTtsStarted:
            if (state_ != VoiceInteractionState::kListening && state_ != VoiceInteractionState::kThinking) {
                return InvalidTransition(state_, event);
            }
            state_ = VoiceInteractionState::kSpeaking;
            break;
        case VoiceInteractionEvent::kTtsStopped:
            if (state_ != VoiceInteractionState::kSpeaking) return InvalidTransition(state_, event);
            state_ = VoiceInteractionState::kStandby;
            transition.action = VoiceInteractionAction::kRestoreStandby;
            break;
        case VoiceInteractionEvent::kInterruptRequested:
            if (state_ != VoiceInteractionState::kListening && state_ != VoiceInteractionState::kThinking &&
                state_ != VoiceInteractionState::kSpeaking) {
                return InvalidTransition(state_, event);
            }
            state_ = VoiceInteractionState::kInterrupting;
            transition.action = VoiceInteractionAction::kInterruptSession;
            break;
        case VoiceInteractionEvent::kInterruptCompleted:
            if (state_ != VoiceInteractionState::kInterrupting) return InvalidTransition(state_, event);
            state_ = VoiceInteractionState::kStandby;
            transition.action = VoiceInteractionAction::kRestoreStandby;
            break;
        case VoiceInteractionEvent::kStandbyReady:
            if (state_ != VoiceInteractionState::kStandby && state_ != VoiceInteractionState::kError &&
                state_ != VoiceInteractionState::kInterrupting) {
                return InvalidTransition(state_, event);
            }
            state_ = VoiceInteractionState::kStandby;
            break;
        case VoiceInteractionEvent::kTransportDisconnected:
            if (state_ == VoiceInteractionState::kBooting || state_ == VoiceInteractionState::kError) {
                return InvalidTransition(state_, event);
            }
            state_ = VoiceInteractionState::kReconnecting;
            transition.action = VoiceInteractionAction::kRestoreStandby;
            break;
        case VoiceInteractionEvent::kTransportConnected:
            if (state_ != VoiceInteractionState::kReconnecting) return InvalidTransition(state_, event);
            state_ = VoiceInteractionState::kStandby;
            transition.action = VoiceInteractionAction::kRestoreStandby;
            break;
        case VoiceInteractionEvent::kFailure:
            if (state_ == VoiceInteractionState::kBooting) return InvalidTransition(state_, event);
            if (state_ == VoiceInteractionState::kError) break;
            state_ = VoiceInteractionState::kError;
            transition.action = VoiceInteractionAction::kInterruptSession;
            break;
    }
    transition.state = state_;
    return Result<VoiceInteractionTransition>::Success(transition);
}

VoiceInteractionState VoiceInteractionController::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string_view VoiceInteractionController::display_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
        case VoiceInteractionState::kBooting:
            return "BOOT";
        case VoiceInteractionState::kStandby:
            return "IDLE";
        case VoiceInteractionState::kListening:
            return "LISTEN";
        case VoiceInteractionState::kThinking:
            return "THINK";
        case VoiceInteractionState::kSpeaking:
            return "SPEAK";
        case VoiceInteractionState::kInterrupting:
            return "STOP";
        case VoiceInteractionState::kReconnecting:
            return "RECONNECT";
        case VoiceInteractionState::kError:
            return "ERROR";
    }
    return "ERROR";
}

}  // namespace voicelife::voice
