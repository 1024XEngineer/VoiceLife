#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "voicelife/voice/voice_ports.h"

namespace voicelife::linx {

struct LinxConnectionConfig {
    std::string websocket_url;
    // A reference such as secret://linx/device-token. The resolved token is
    // owned by the platform transport and never enters this component's logs.
    std::string token_ref;
    std::string device_id;
    std::string client_id;
    std::optional<std::string> agent_id;

    [[nodiscard]] bool valid() const {
        return !websocket_url.empty() && !token_ref.empty() && !device_id.empty() &&
               !client_id.empty();
    }
};

struct LinxAudioParams {
    voice::AudioCodec codec = voice::AudioCodec::kPcmS16Le;
    uint32_t sample_rate_hz = 16000;
    uint8_t channels = 1;
    uint8_t bits_per_sample = 16;
    uint16_t frame_duration_ms = 20;

    [[nodiscard]] bool valid() const {
        return sample_rate_hz > 0 && channels > 0 && bits_per_sample > 0 &&
               frame_duration_ms > 0;
    }
};

enum class LinxMessageKind { kHello, kStt, kTts, kError };
enum class LinxTtsState { kStart, kSentenceStart, kStop };

struct LinxInboundMessage {
    LinxMessageKind kind = LinxMessageKind::kError;
    std::optional<std::string> session_id;
    std::optional<LinxAudioParams> audio_params;
    std::optional<LinxTtsState> tts_state;
    std::string text;
    bool aborted = false;
};

struct LinxTransportSink {
    std::function<void(std::string_view)> on_text;
    std::function<void(const std::vector<uint8_t>&)> on_binary;
};

class LinxTransportPort {
   public:
    virtual ~LinxTransportPort() = default;
    virtual Status Connect(const LinxConnectionConfig& config, LinxTransportSink sink) = 0;
    virtual Status SendText(std::string_view message) = 0;
    virtual Status SendAudio(const voice::AudioFrame& frame) = 0;
    virtual Status Close() = 0;
};

class LinxProtocolCodecPort {
   public:
    virtual ~LinxProtocolCodecPort() = default;
    virtual Result<std::string> EncodeHello(const voice::VoiceSessionConfig& config,
                                            const LinxConnectionConfig& connection) const = 0;
    virtual Result<std::string> EncodeListenStart(const voice::VoiceSessionConfig& config) const = 0;
    virtual Result<std::string> EncodeListenStop(const voice::VoiceSessionConfig& config) const = 0;
    virtual Result<std::string> EncodeListenDetect(const voice::VoiceSessionConfig& config,
                                                   std::string_view text) const = 0;
    virtual Result<std::string> EncodeAbort(const voice::VoiceSessionConfig& config,
                                            std::string_view reason) const = 0;
    virtual Result<LinxInboundMessage> DecodeText(std::string_view message) const = 0;
};

// Portable codec used by host tests and by the ESP adapter until cJSON is
// wired into the transport component. It accepts only the documented XRobot
// message shape and rejects unknown or malformed control messages.
class LinxJsonCodec final : public LinxProtocolCodecPort {
   public:
    Result<std::string> EncodeHello(const voice::VoiceSessionConfig& config,
                                    const LinxConnectionConfig& connection) const override;
    Result<std::string> EncodeListenStart(const voice::VoiceSessionConfig& config) const override;
    Result<std::string> EncodeListenStop(const voice::VoiceSessionConfig& config) const override;
    Result<std::string> EncodeListenDetect(const voice::VoiceSessionConfig& config,
                                           std::string_view text) const override;
    Result<std::string> EncodeAbort(const voice::VoiceSessionConfig& config,
                                    std::string_view reason) const override;
    Result<LinxInboundMessage> DecodeText(std::string_view message) const override;
};

}  // namespace voicelife::linx
