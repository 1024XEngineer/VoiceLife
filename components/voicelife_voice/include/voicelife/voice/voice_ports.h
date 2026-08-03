#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::voice {

using VoiceEventSink = std::function<void(const VoiceEvent&)>;
using EvidenceSink = std::function<void(const VoiceEvidence&)>;

class AudioInputPort {
   public:
    virtual ~AudioInputPort() = default;
    virtual Status Open(const AudioFormat& format) = 0;
    virtual Status StartCapture(VoiceMode mode) = 0;
    virtual Status StopCapture() = 0;
    virtual void Close() = 0;
};

class AudioOutputPort {
   public:
    virtual ~AudioOutputPort() = default;
    virtual Status Open(const AudioFormat& format) = 0;
    virtual Status Push(const AudioFrame& frame) = 0;
    virtual Status Flush() = 0;
    virtual void Close() = 0;
};

class VoiceTransportPort {
   public:
    virtual ~VoiceTransportPort() = default;
    virtual Status Connect(const VoiceSessionConfig& config, VoiceEventSink sink) = 0;
    virtual Status SendText(std::string_view message) = 0;
    virtual Status SendAudio(const AudioFrame& frame) = 0;
    virtual Status Close() = 0;
};

// 旧协调器的生命周期 Port，保留这一小组方法以便迁移期 Adapter 可以逐步替换。
class SpeechProviderPort {
   public:
    virtual ~SpeechProviderPort() = default;
    virtual Status Connect() = 0;
    virtual void Disconnect() = 0;
};

class CodecStrategy {
   public:
    virtual ~CodecStrategy() = default;
    [[nodiscard]] virtual AudioCodec codec() const = 0;
    virtual Result<AudioFrame> Encode(const AudioFrame& pcm) = 0;
    virtual Result<AudioFrame> Decode(const AudioFrame& encoded) = 0;
};

// 协议防腐层：Provider 把外部 STT/TTS 字段映射为稳定的语音语义。
class ASRAdapter {
   public:
    virtual ~ASRAdapter() = default;
    virtual Status OnEvent(const VoiceEvent& event) = 0;
};

class TTSAdapter {
   public:
    virtual ~TTSAdapter() = default;
    virtual Status Speak(std::string_view text) = 0;
    virtual Status OnEvent(const VoiceEvent& event) = 0;
};

class RealtimeAdapter {
   public:
    virtual ~RealtimeAdapter() = default;
    virtual Status Begin(VoiceMode mode) = 0;
    virtual Status Interrupt() = 0;
};

class SpeechProviderAdapter {
   public:
    virtual ~SpeechProviderAdapter() = default;
    virtual Status Connect(const VoiceSessionConfig& config, VoiceEventSink sink) = 0;
    virtual Status StartCapture(VoiceMode mode) = 0;
    virtual Status StopCapture() = 0;
    virtual Status SendAudio(const AudioFrame& frame) = 0;
    virtual Status Abort(std::string_view reason) = 0;
    virtual Status Speak(std::string_view text) = 0;
    virtual Status Disconnect() = 0;
    [[nodiscard]] virtual const CapabilityProfile& capabilities() const = 0;
};

using SpeechProviderFactory = std::function<std::unique_ptr<SpeechProviderAdapter>()>;

class SpeechProviderRegistry {
   public:
    static constexpr std::size_t kMaxProviders = 8;
    static SpeechProviderRegistry& Instance();

    Status Register(std::string provider_id, CapabilityProfile profile, SpeechProviderFactory factory);
    Result<std::unique_ptr<SpeechProviderAdapter>> Create(
        std::string_view provider_id, const std::vector<std::string>& required_capabilities) const;

   private:
    SpeechProviderRegistry() = default;
    struct Entry {
        std::string provider_id;
        CapabilityProfile profile;
        SpeechProviderFactory factory;
    };
    std::array<Entry, kMaxProviders> entries_{};
    std::size_t size_ = 0;
};

}  // namespace voicelife::voice
