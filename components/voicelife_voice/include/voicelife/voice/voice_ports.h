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

/// Callback that receives VoiceEvents from the speech provider transport layer.
using VoiceEventSink = std::function<void(const VoiceEvent&)>;

/// Callback for diagnostic evidence emitted during session lifecycle transitions.
using EvidenceSink = std::function<void(const VoiceEvidence&)>;

/// Callback for raw audio frames from the provider (downlink) or input port (uplink).
using AudioFrameSink = std::function<Status(AudioFrame)>;

/// Abstraction for a hardware audio capture device (I2S microphone, AFE pipeline, etc.).
class AudioInputPort {
   public:
    /// Virtual destructor.
    virtual ~AudioInputPort() = default;

    /// Set the callback that receives captured audio frames from this port.
    virtual void SetAudioSink(AudioFrameSink sink) = 0;

    /// Open the capture device with the negotiated uplink audio format.
    virtual Status Open(const AudioFormat& format) = 0;

    /// Start delivering captured frames to the configured sink.
    virtual Status StartCapture(VoiceMode mode) = 0;

    /// Stop capture. Late frames arriving after this call are rejected by the session.
    virtual Status StopCapture() = 0;

    /// Release hardware resources and clear the sink callback.
    virtual void Close() = 0;
};

/// Abstraction for a hardware audio playback device (I2S speaker, DAC, etc.).
class AudioOutputPort {
   public:
    /// Virtual destructor.
    virtual ~AudioOutputPort() = default;

    /// Open the playback device with the negotiated downlink audio format.
    virtual Status Open(const AudioFormat& format) = 0;

    /// Push a decoded audio frame into the playback queue.
    virtual Status Push(const AudioFrame& frame) = 0;

    /// Drop all buffered frames. Called on interrupt or generation invalidation.
    virtual Status Flush() = 0;

    /// Release hardware resources.
    virtual void Close() = 0;
};

/// Low-level transport port for a voice session (WebSocket, TCP, etc.).
class VoiceTransportPort {
   public:
    /// Virtual destructor.
    virtual ~VoiceTransportPort() = default;

    /// Establish the transport connection and register event/text/audio callbacks.
    virtual Status Connect(const VoiceSessionConfig& config, VoiceEventSink sink) = 0;

    /// Send a text control message over the transport.
    virtual Status SendText(std::string_view message) = 0;

    /// Send an audio frame over the transport.
    virtual Status SendAudio(const AudioFrame& frame) = 0;

    /// Tear down the transport connection.
    virtual Status Close() = 0;
};

/// Legacy lifecycle port retained during migration. New code should use SpeechProviderAdapter.
class SpeechProviderPort {
   public:
    /// Virtual destructor.
    virtual ~SpeechProviderPort() = default;
    /// Establish connection.
    virtual Status Connect() = 0;
    /// Tear down connection.
    virtual void Disconnect() = 0;
};

/// Encoding/decoding strategy for a specific audio codec (PCM, Opus, etc.).
class CodecStrategy {
   public:
    /// Virtual destructor.
    virtual ~CodecStrategy() = default;

    /// The codec this strategy handles.
    [[nodiscard]] virtual AudioCodec codec() const = 0;

    /// Encode a PCM frame into the codec's compressed format.
    virtual Result<AudioFrame> Encode(const AudioFrame& pcm) = 0;

    /// Decode a compressed frame back to PCM.
    virtual Result<AudioFrame> Decode(const AudioFrame& encoded) = 0;
};

/// Maps provider-specific ASR events into the stable VoiceEvent vocabulary.
class ASRAdapter {
   public:
    /// Virtual destructor.
    virtual ~ASRAdapter() = default;
    /// Forward an ASR event.
    virtual Status OnEvent(const VoiceEvent& event) = 0;
};

/// Maps provider-specific TTS events into the stable VoiceEvent vocabulary.
class TTSAdapter {
   public:
    /// Virtual destructor.
    virtual ~TTSAdapter() = default;
    /// Speak the given text.
    virtual Status Speak(std::string_view text) = 0;
    /// Forward a TTS event.
    virtual Status OnEvent(const VoiceEvent& event) = 0;
};

/// Adapter for real-time streaming protocols that need explicit begin/interrupt signalling.
class RealtimeAdapter {
   public:
    /// Virtual destructor.
    virtual ~RealtimeAdapter() = default;
    /// Begin a realtime session.
    virtual Status Begin(VoiceMode mode) = 0;
    /// Interrupt the current operation.
    virtual Status Interrupt() = 0;
};

/// Full speech provider abstraction. A concrete implementation (Linx, xiaozhi, etc.)
/// wraps a transport, codec, and protocol logic behind this single interface.
class SpeechProviderAdapter {
   public:
    virtual ~SpeechProviderAdapter() = default;

    /// Optional during migration. Providers with downlink audio call this sink for
    /// each decoded frame; the session owns generation checks.
    virtual void SetAudioSink(AudioFrameSink) {}

    /// Notify the provider of the current connection epoch. Late frames with an
    /// older generation are rejected.
    virtual void SetGeneration(uint64_t) {}

    /// Establish the provider connection and register the event callback.
    virtual Status Connect(const VoiceSessionConfig& config, VoiceEventSink sink) = 0;

    /// Start upstream audio capture on the provider side.
    virtual Status StartCapture(VoiceMode mode) = 0;

    /// Stop upstream audio capture.
    virtual Status StopCapture() = 0;

    /// Send an audio frame to the provider.
    virtual Status SendAudio(const AudioFrame& frame) = 0;

    /// Abort the current operation (playback or capture) with a reason.
    virtual Status Abort(std::string_view reason) = 0;

    /// Request TTS playback of the given text.
    virtual Status Speak(std::string_view text) = 0;

    /// Tear down the provider connection.
    virtual Status Disconnect() = 0;

    /// Available after Connect() completes. Returns the duplex audio formats
    /// negotiated during the server hello handshake.
    [[nodiscard]] virtual Result<VoiceAudioFormats> audio_formats() const = 0;

    /// The capability profile declared at registration time.
    [[nodiscard]] virtual const CapabilityProfile& capabilities() const = 0;
};

/// Factory function that creates a SpeechProviderAdapter instance.
using SpeechProviderFactory = std::function<std::unique_ptr<SpeechProviderAdapter>()>;

/// Fixed-capacity provider registry. All Register() calls must complete before
/// the RTOS scheduler starts; runtime queries via Create() are read-only.
///
/// Thread safety: no internal mutex. Callers are responsible for init-time ordering.
class SpeechProviderRegistry {
   public:
    /// Maximum number of registered providers.
    static constexpr std::size_t kMaxProviders = 16;

    /// Global singleton.
    static SpeechProviderRegistry& Instance();

    /// Register a provider factory with its capability profile.
    /// Must be called before the scheduler starts.
    Status Register(std::string provider_id, CapabilityProfile profile, SpeechProviderFactory factory);

    /// Create a provider instance matching the given id and required capabilities.
    Result<std::unique_ptr<SpeechProviderAdapter>> Create(std::string_view provider_id,
                                                          const std::vector<std::string>& required_capabilities) const;

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
