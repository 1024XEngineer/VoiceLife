#pragma once

#include <mutex>
#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::voice {

/** @brief 小智式单轮语音交互在板端可见的状态。 */
enum class VoiceInteractionState {
    kBooting,
    kStandby,
    /** 采集请求已提交，等待 capture_started 确认（事务式启动，避免假"聆听中"）。 */
    kOpeningCapture,
    kListening,
    /** 语音端点已检测到（VAD 静音）：已发 listen.stop，等待最终 STT。 */
    kFinalizing,
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
    /** VAD 检测到语音端点（说话结束）：发 listen.stop，等待最终 STT，不回待机。 */
    kEndpointDetected,
    /** 最终 STT 超时：kFinalizing → kStandby，中止残留服务端回合并恢复待机。 */
    kFinalizationTimedOut,
    /** 告别（再见/拜拜）回复播报完成：kSpeaking → kStandby，恢复待机。 */
    kFarewellCompleted,
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
    kRestoreStandby,
    kInterruptSession,
};

/** @brief 单个交互事件处理后的状态与动作。 */
struct VoiceInteractionTransition {
    VoiceInteractionState state = VoiceInteractionState::kBooting;
    VoiceInteractionAction action = VoiceInteractionAction::kNone;
};

/** @brief 牛头表情键（显示模型层使用，与 OLED 渲染解耦）。 */
enum class VoiceMood {
    kNeutral,
    kHappy,
    kSad,
    kThinking,
    kSurprised,
    kSpeaking,
    kAngry,
};

/** @brief 内容栏当前展示的文本角色。 */
enum class VoiceContentRole {
    kNone,
    kSystem,
    kUser,
    kAssistant,
};

/**
 * @brief 显示模型快照：一次会话阶段变化后派生出的完整可见状态。
 * 由 Runtime 维护，仅在 revision 变化时提交给渲染器，避免全屏重绘。
 */
struct DisplaySnapshot {
    VoiceInteractionState phase = VoiceInteractionState::kBooting;
    VoiceMood mood = VoiceMood::kNeutral;
    /** 上行状态栏文本（如“聆听中...”“处理中...”）。 */
    std::string status_text;
    /** 下行内容栏文本（用户语音 / 助手回复 / 系统提示）。 */
    std::string content_text;
    VoiceContentRole role = VoiceContentRole::kNone;
    uint64_t revision = 0;
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
