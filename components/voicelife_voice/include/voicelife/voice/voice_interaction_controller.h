#pragma once

#include <mutex>
#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::voice {

/** @brief 小智式单轮语音交互在板端可见的状态。 */
enum class VoiceInteractionState {
    kBooting,
    kStandby,
    kListening,
    kThinking,
    kSpeaking,
    kInterrupting,
    kReconnecting,
    kError,
};

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
    kCaptureStarted,
    kIntentReceived,
    kTtsStarted,
    kTtsStopped,
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
    /** 打断当前会话后重新发送本地唤醒事件并开始采集。 */
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
    /** @brief 返回适合 OLED 等窄屏的稳定短文本。 @return 当前状态文本。 */
    [[nodiscard]] std::string_view display_text() const;

   private:
    mutable std::mutex mutex_;
    VoiceInteractionState state_ = VoiceInteractionState::kBooting;
};

}  // namespace voicelife::voice
