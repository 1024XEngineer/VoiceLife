#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace voicelife::voice {

/** 表示语音链路使用的音频编码格式。 */
enum class AudioCodec { kPcmS16Le, kOpus };

/** 表示语音会话的采集触发模式。 */
enum class VoiceMode { kManual, kAuto, kRealtime };

/** 表示语音会话当前所处的生命周期状态。 */
enum class VoiceSessionState { kStopped, kStarting, kReady, kCapturing, kSpeaking, kFailed };

/** 表示 Provider 发出的语音领域事件类型。 */
enum class VoiceEventKind {
    kConnected,
    kAsrText,
    kTtsStarted,
    kTtsSentenceStarted,
    kTtsStopped,
    kToolCall,
    kError,
};

/** 描述一条音频流的编码、采样和帧时长约束。 */
struct AudioFormat {
    AudioCodec codec = AudioCodec::kPcmS16Le;
    uint32_t sample_rate_hz = 16000;
    uint8_t channels = 1;
    uint8_t bits_per_sample = 16;
    uint16_t frame_duration_ms = 20;

    /**
     * @brief 校验音频参数是否可以用于设备或传输。
     * @return 参数有效时返回 true。
     */
    [[nodiscard]] bool valid() const {
        return sample_rate_hz > 0 && channels > 0 && bits_per_sample > 0 && frame_duration_ms > 0;
    }
};

/** 携带一帧带有会话代次和序号的音频数据。 */
struct AudioFrame {
    uint64_t generation = 0;
    uint64_t sequence = 0;
    AudioFormat format;
    std::vector<uint8_t> payload;
};

/** 保存一次语音会话的 Provider、模式、超时和代次配置。 */
struct VoiceSessionConfig {
    std::string session_id;
    std::string provider_id;
    VoiceMode mode = VoiceMode::kManual;
    AudioFormat audio;
    uint32_t hello_timeout_ms = 10000;
    uint32_t reconnect_backoff_ms = 250;
    bool enable_mcp = true;
    // Assigned by VoiceSession for every connection epoch. Providers must
    // copy it onto asynchronous events and downlink audio frames.
    uint64_t generation = 0;
};

/** 描述 Provider 声明的能力集合。 */
struct CapabilityProfile {
    std::string provider_id;
    std::vector<std::string> capabilities;

    /**
     * @brief 判断 Provider 是否声明了指定能力。
     * @param capability 待查询的能力名称。
     * @return 已声明该能力时返回 true。
     */
    [[nodiscard]] bool Has(std::string_view capability) const {
        return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
    }
};

/** 表示一次会话状态、识别、合成或错误事件。 */
struct VoiceEvent {
    VoiceEventKind kind = VoiceEventKind::kError;
    uint64_t generation = 0;
    std::string text;
    bool aborted = false;
};

/** 保存可审计的语音会话事件证据。 */
struct VoiceEvidence {
    std::string session_id;
    uint64_t generation = 0;
    std::string event;
    std::string detail;
};

}  // namespace voicelife::voice
