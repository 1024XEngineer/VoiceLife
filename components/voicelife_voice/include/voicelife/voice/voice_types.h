#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace voicelife::voice {

/** @brief 音频编解码器类型。 */
enum class AudioCodec { kPcmS16Le, kOpus };

/** @brief 语音会话模式。 */
enum class VoiceMode { kManual, kAuto, kRealtime };

/** @brief 语音会话状态。 */
enum class VoiceSessionState { kStopped, kStarting, kReady, kCapturing, kSpeaking, kFailed };

/** @brief 语音事件类型。 */
enum class VoiceEventKind {
    /** @brief 已连接。 */
    kConnected,
    /** @brief 已断开。 */
    kDisconnected,
    /** @brief ASR 文本。 */
    kAsrText,
    /** @brief TTS 开始。 */
    kTtsStarted,
    /** @brief TTS 句子开始。 */
    kTtsSentenceStarted,
    /** @brief TTS 停止。 */
    kTtsStopped,
    /** @brief 工具调用。 */
    kToolCall,
    /** @brief 错误。 */
    kError,
};

/** @brief 音频格式：编码、采样率、声道与位深。 */
struct AudioFormat {
    /** @brief 编解码器。 */
    AudioCodec codec = AudioCodec::kPcmS16Le;
    /** @brief 采样率（Hz）。 */
    uint32_t sample_rate_hz = 16000;
    /** @brief 声道数。 */
    uint8_t channels = 1;
    /** @brief 每样本比特数。 */
    uint8_t bits_per_sample = 16;
    /** @brief 帧时长（毫秒）。 */
    uint16_t frame_duration_ms = 20;

    /** @brief 格式字段是否全部有效。 @return 有效返回 true。 */
    [[nodiscard]] bool valid() const {
        return sample_rate_hz > 0 && channels > 0 && bits_per_sample > 0 && frame_duration_ms > 0;
    }
};

/** @brief 音频帧：携带代次、序号、格式与载荷。 */
struct AudioFrame {
    /**
     * @brief 单帧载荷的绝对上限（字节）。
     *
     * 覆盖 24 kHz 32 位立体声 60 ms PCM（约 11.5 KB）并留余量。
     */
    static constexpr size_t kMaxPayloadBytes = 16384;

    /** @brief 连接代次。 */
    uint64_t generation = 0;
    /** @brief 帧序号。 */
    uint64_t sequence = 0;
    /** @brief 帧格式。 */
    AudioFormat format;
    /** @brief 帧载荷。 */
    std::vector<uint8_t> payload;
};

/**
 * @brief 双向音频格式。
 *
 * 采集与播放分离，因为语音服务可能接受 16 kHz 上行，
 * 而在同一连接上返回 24 kHz TTS 音频。
 */
struct VoiceAudioFormats {
    /** @brief 采集格式。 */
    AudioFormat capture;
    /** @brief 播放格式。 */
    AudioFormat playback;

    /** @brief 双向格式是否均有效。 @return 有效返回 true。 */
    [[nodiscard]] bool valid() const { return capture.valid() && playback.valid(); }
};

/** @brief 语音会话配置。 */
struct VoiceSessionConfig {
    /** @brief 会话 ID。 */
    std::string session_id;
    /** @brief Provider ID。 */
    std::string provider_id;
    /** @brief 会话模式。 */
    VoiceMode mode = VoiceMode::kManual;
    /** @brief 音频格式。 */
    AudioFormat audio;
    /** @brief hello 握手超时（毫秒）。 */
    uint32_t hello_timeout_ms = 10000;
    /** @brief 重连退避（毫秒）。 */
    uint32_t reconnect_backoff_ms = 250;
    /** @brief 是否启用 MCP。 */
    bool enable_mcp = true;
    /**
     * @brief 当前连接代次。
     *
     * 由 VoiceSession 为每个连接 epoch 分配，Provider 必须将其
     * 复制到异步事件与下行音频帧。
     */
    uint64_t generation = 0;
};

/** @brief Provider 能力声明。 */
struct CapabilityProfile {
    /** @brief Provider ID。 */
    std::string provider_id;
    /** @brief 能力列表。 */
    std::vector<std::string> capabilities;

    /** @brief 是否声明了指定能力。 @param capability 能力名。 @return 声明返回 true。 */
    [[nodiscard]] bool Has(std::string_view capability) const {
        const auto found = std::find(capabilities.begin(), capabilities.end(), capability);
        return found != capabilities.end();
    }
};

/** @brief 语音事件：Provider 传输层上报给会话。 */
struct VoiceEvent {
    /** @brief 事件类型。 */
    VoiceEventKind kind = VoiceEventKind::kError;
    /** @brief 代次。 */
    uint64_t generation = 0;
    /** @brief 事件文本。 */
    std::string text;
    /** @brief 是否被中止。 */
    bool aborted = false;
};

/** @brief 会话诊断证据：生命周期事件的可追踪记录。 */
struct VoiceEvidence {
    /** @brief 会话 ID。 */
    std::string session_id;
    /** @brief 代次。 */
    uint64_t generation = 0;
    /** @brief 事件名。 */
    std::string event;
    /** @brief 详情。 */
    std::string detail;
};

}  // namespace voicelife::voice
