#pragma once

#include <mutex>
#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/display_snapshot.h"
#include "voicelife/voice/voice_interaction_state.h"

namespace voicelife::voice {

/** @brief 板端输入或语音会话生命周期产生的交互事件。 */
enum class VoiceInteractionEvent {
    kBootCompleted,
    /** BOOT 单击：待机开始、聆听结束、播报打断。 */
    kToggleChat,
    /** 触摸按下：开始手动聆听，播报中先打断再聆听。 */
    kPressDown,
    /** 触摸松开：结束手动聆听。 */
    kPressUp,
    kWakeDetected,
    /** 打断确认已成功提交给 Provider：kInterrupting → kListening。 */
    kInterruptAcknowledged,
    kCaptureStarted,
    /** 本地唤醒确认未在限定时间内产生首段音频：跳过迟到播报并开始采集。 */
    kAcknowledgementTimedOut,
    /** VAD 检测到语音端点（说话结束）：发 listen.stop，等待最终 STT，不回待机。 */
    kEndpointDetected,
    /** 最终 STT 超时：kFinalizing → kStandby，中止残留服务端回合并恢复待机。 */
    kFinalizationTimedOut,
    /** 无需 follow-up 的终结型回复播报完成：kSpeaking → kStandby，恢复待机。 */
    kTerminalResponseCompleted,
    kIntentReceived,
    kTtsStarted,
    kTtsStopped,
    /** 本地“别说了”：取消旧播报后播放一次确认，并在确认结束后直接聆听。 */
    kInterruptAndAcknowledge,
    kInterruptRequested,
    kInterruptCompleted,
    kStandbyReady,
    kTransportDisconnected,
    kTransportConnected,
    kFailure,
};

/** @brief 合法状态迁移后由 Runtime 执行的动作。 */
enum class VoiceInteractionAction {
    kNone,
    kStartCapture,
    kStartVoiceTurn,
    kStopVoiceTurn,
    /** 打断当前会话后开始手动采集，不发送本地唤醒事件。 */
    kInterruptAndStartCapture,
    /** 向 Provider 提交本地唤醒确认 TTS；播报结束后再开始采集。 */
    kInterruptAndStartVoiceTurn,
    kRestoreStandby,
    kInterruptSession,
};

/** @brief 单个交互事件处理后的状态与动作。 */
struct VoiceInteractionTransition {
    VoiceInteractionState state = VoiceInteractionState::kBooting;
    VoiceInteractionAction action = VoiceInteractionAction::kNone;
};

/**
 * @brief 保持板端 UI 与用户控制和 VoiceSession 同步。
 *
 * 核心不引入 ESP-IDF 或 Provider 专有状态。
 */
class VoiceInteractionController {
   public:
    /**
     * @brief 处理一次板端输入或会话生命周期事件。
     * @param event 要处理的交互事件。
     * @return 状态迁移和 Runtime 动作；非法迁移返回 Conflict 且保留原状态。
     */
    Result<VoiceInteractionTransition> Handle(VoiceInteractionEvent event);

    /** @brief 返回当前板端可见状态。 @return 当前交互状态。 */
    [[nodiscard]] VoiceInteractionState state() const;

   private:
    mutable std::mutex mutex_;
    VoiceInteractionState state_ = VoiceInteractionState::kBooting;
};

}  // namespace voicelife::voice
