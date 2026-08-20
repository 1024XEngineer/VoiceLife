#pragma once

namespace voicelife::voice {

/** @brief 小智式单轮语音交互在板端可见的状态。 */
enum class VoiceInteractionState {
    kBooting,
    kStandby,
    /** 本地唤醒已提交确认播报，尚未打开麦克风。 */
    kAcknowledging,
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

}  // namespace voicelife::voice
