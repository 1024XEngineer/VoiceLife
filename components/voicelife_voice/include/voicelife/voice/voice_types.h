#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace voicelife::voice {

enum class AudioCodec { kPcmS16Le, kOpus };

enum class VoiceMode { kManual, kAuto, kRealtime };

enum class VoiceSessionState { kStopped, kStarting, kReady, kCapturing, kSpeaking, kFailed };

enum class VoiceEventKind {
    kConnected,
    kAsrText,
    kTtsStarted,
    kTtsSentenceStarted,
    kTtsStopped,
    kToolCall,
    kError,
};

struct AudioFormat {
    AudioCodec codec = AudioCodec::kPcmS16Le;
    uint32_t sample_rate_hz = 16000;
    uint8_t channels = 1;
    uint8_t bits_per_sample = 16;
    uint16_t frame_duration_ms = 20;

    [[nodiscard]] bool valid() const {
        return sample_rate_hz > 0 && channels > 0 && bits_per_sample > 0 && frame_duration_ms > 0;
    }
};

struct AudioFrame {
    uint64_t generation = 0;
    uint64_t sequence = 0;
    AudioFormat format;
    std::vector<uint8_t> payload;
};

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

struct CapabilityProfile {
    std::string provider_id;
    std::vector<std::string> capabilities;

    [[nodiscard]] bool Has(std::string_view capability) const {
        for (const std::string& item : capabilities) {
            if (item == capability) {
                return true;
            }
        }
        return false;
    }
};

struct VoiceEvent {
    VoiceEventKind kind = VoiceEventKind::kError;
    uint64_t generation = 0;
    std::string text;
    bool aborted = false;
};

struct VoiceEvidence {
    std::string session_id;
    uint64_t generation = 0;
    std::string event;
    std::string detail;
};

}  // namespace voicelife::voice
