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
                // 事务式启动：先提交采集请求（kOpeningCapture），
                // 收到 capture_started 才进入 kListening，避免假"聆听中"。
                state_ = VoiceInteractionState::kOpeningCapture;
                transition.action = VoiceInteractionAction::kStartCapture;
            } else if (state_ == VoiceInteractionState::kListening) {
                // BOOT 短按停止：进 kFinalizing（发 listen.stop 等最终 STT），
                // 由最终 STT/5s 超时收尾，不直接回 Standby。
                state_ = VoiceInteractionState::kFinalizing;
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
                // 事务式启动：先提交采集请求，capture_started 后才进入 kListening。
                state_ = VoiceInteractionState::kOpeningCapture;
                transition.action = VoiceInteractionAction::kStartCapture;
            } else if (state_ == VoiceInteractionState::kSpeaking || state_ == VoiceInteractionState::kThinking) {
                state_ = VoiceInteractionState::kListening;
                transition.action = VoiceInteractionAction::kInterruptAndStartCapture;
            } else {
                return InvalidTransition(state_, event);
            }
            break;
        case VoiceInteractionEvent::kPressUp:
            // 触摸松开/输入结束：Listening → kFinalizing（停止采集发 listen.stop，
            // 等待最终 STT），由最终 STT 或 5s 超时收尾，不直接回 Standby。
            if (state_ != VoiceInteractionState::kListening) return InvalidTransition(state_, event);
            state_ = VoiceInteractionState::kFinalizing;
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
            // 事务式启动确认：kOpeningCapture → kListening（采集真正开始）；
            // kListening 幂等（重复 capture_started 无害）。
            if (state_ == VoiceInteractionState::kOpeningCapture) {
                state_ = VoiceInteractionState::kListening;
            } else if (state_ != VoiceInteractionState::kListening) {
                return InvalidTransition(state_, event);
            }
            break;
        case VoiceInteractionEvent::kEndpointDetected:
            // VAD 检测到说话结束：停止采集并发送 listen.stop（kStopVoiceTurn），
            // 但进入 kFinalizing 等待服务端最终 STT，不直接回待机。
            if (state_ != VoiceInteractionState::kListening) return InvalidTransition(state_, event);
            state_ = VoiceInteractionState::kFinalizing;
            transition.action = VoiceInteractionAction::kStopVoiceTurn;
            break;
        case VoiceInteractionEvent::kFinalizationTimedOut:
            // 最终 STT 超时：kFinalizing → kStandby，恢复待机（由 runtime 中止残留回合）。
            if (state_ != VoiceInteractionState::kFinalizing) return InvalidTransition(state_, event);
            state_ = VoiceInteractionState::kStandby;
            transition.action = VoiceInteractionAction::kRestoreStandby;
            break;
        case VoiceInteractionEvent::kTerminalResponseCompleted:
            // 告别、绑定码等终结型回复播报完成：不进入 follow-up，直接恢复待机。
            if (state_ != VoiceInteractionState::kSpeaking) return InvalidTransition(state_, event);
            state_ = VoiceInteractionState::kStandby;
            transition.action = VoiceInteractionAction::kRestoreStandby;
            break;
        case VoiceInteractionEvent::kIntentReceived:
            if (state_ != VoiceInteractionState::kListening && state_ != VoiceInteractionState::kThinking &&
                state_ != VoiceInteractionState::kFinalizing) {
                return InvalidTransition(state_, event);
            }
            state_ = VoiceInteractionState::kThinking;
            break;
        case VoiceInteractionEvent::kTtsStarted:
            if (state_ != VoiceInteractionState::kListening && state_ != VoiceInteractionState::kThinking &&
                state_ != VoiceInteractionState::kFinalizing) {
                return InvalidTransition(state_, event);
            }
            state_ = VoiceInteractionState::kSpeaking;
            break;
        case VoiceInteractionEvent::kTtsStopped:
            if (state_ != VoiceInteractionState::kSpeaking) return InvalidTransition(state_, event);
            // 播报结束后进入 follow-up 聆听：保持开麦让用户可直接续说，
            // 无需重新唤醒。长时间无输入由聆听超时结束本回合。
            state_ = VoiceInteractionState::kListening;
            transition.action = VoiceInteractionAction::kStartCapture;
            break;
        case VoiceInteractionEvent::kInterruptAndAcknowledge:
            // “别说了”不是静默终止：先隔离正在播放的旧回合，再经 Provider
            // 合成一次“收到！”。TTS 结束后沿 kTtsStopped 正常进入聆听。
            if (state_ != VoiceInteractionState::kListening && state_ != VoiceInteractionState::kThinking &&
                state_ != VoiceInteractionState::kSpeaking && state_ != VoiceInteractionState::kFinalizing) {
                return InvalidTransition(state_, event);
            }
            state_ = VoiceInteractionState::kListening;
            transition.action = VoiceInteractionAction::kInterruptAndStartVoiceTurn;
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
                state_ != VoiceInteractionState::kInterrupting && state_ != VoiceInteractionState::kOpeningCapture) {
                return InvalidTransition(state_, event);
            }
            state_ = VoiceInteractionState::kStandby;
            break;
        case VoiceInteractionEvent::kTransportDisconnected:
            if (state_ == VoiceInteractionState::kBooting || state_ == VoiceInteractionState::kError) {
                return InvalidTransition(state_, event);
            }
            // 服务端在“牛牛走了～”等固定播报后可以有序结束 WebSocket。
            // 此时本地命令检测仍可用，不能把空闲交互 UI 改成“重连中”；连接在
            // transport 内部后台恢复即可。进行中的回合才需要向用户暴露连接恢复。
            if (state_ == VoiceInteractionState::kStandby) {
                transition.action = VoiceInteractionAction::kRestoreStandby;
                break;
            }
            state_ = VoiceInteractionState::kReconnecting;
            transition.action = VoiceInteractionAction::kRestoreStandby;
            break;
        case VoiceInteractionEvent::kTransportConnected:
            // The provider hello may complete before the runtime posts
            // kBootCompleted. It confirms transport readiness but must not
            // bypass the boot -> standby transition or emit a false rejection.
            if (state_ == VoiceInteractionState::kBooting) break;
            // 后台重连完成，不应让空闲态产生一条无意义的非法状态迁移日志。
            if (state_ == VoiceInteractionState::kStandby) break;
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

}  // namespace voicelife::voice
