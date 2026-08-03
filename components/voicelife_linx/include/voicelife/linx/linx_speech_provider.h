#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "voicelife/linx/linx_types.h"

namespace voicelife::linx {

class LinxSpeechProviderAdapter final : public voice::SpeechProviderAdapter {
   public:
    LinxSpeechProviderAdapter(LinxTransportPort& transport, LinxProtocolCodecPort& codec,
                              LinxConnectionConfig connection,
                              voice::CapabilityProfile capabilities = DefaultCapabilities());

    void SetAudioSink(voice::AudioFrameSink sink) override;
    void SetGeneration(uint64_t generation) override;
    Status Connect(const voice::VoiceSessionConfig& config, voice::VoiceEventSink sink) override;
    Status StartCapture(voice::VoiceMode mode) override;
    Status StopCapture() override;
    Status SendAudio(const voice::AudioFrame& frame) override;
    Status Abort(std::string_view reason) override;
    Status Speak(std::string_view text) override;
    Status Disconnect() override;
    [[nodiscard]] Result<voice::VoiceAudioFormats> audio_formats() const override;
    [[nodiscard]] const voice::CapabilityProfile& capabilities() const override {
        return capabilities_;
    }

    static voice::CapabilityProfile DefaultCapabilities();

   private:
    void OnText(std::string_view message);
    void OnBinary(const std::vector<uint8_t>& payload);
    void OnTransportConnected();
    void OnTransportDisconnected();
    Status Send(Result<std::string> encoded);
    void Emit(voice::VoiceEvent event);

    LinxTransportPort& transport_;
    LinxProtocolCodecPort& codec_;
    LinxConnectionConfig connection_;
    voice::CapabilityProfile capabilities_;
    voice::VoiceSessionConfig config_;
    voice::VoiceEventSink event_sink_;
    voice::AudioFrameSink audio_sink_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> output_sequence_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> transport_connected_{false};
    std::atomic<bool> explicit_disconnect_{false};
    mutable std::mutex callback_mutex_;
    mutable std::mutex hello_mutex_;
    std::condition_variable hello_cv_;
    bool hello_received_ = false;
    bool audio_formats_ready_ = false;
    bool has_negotiated_formats_ = false;
    voice::VoiceAudioFormats audio_formats_;
    voice::VoiceAudioFormats last_audio_formats_;
    Status hello_status_ = Status::Ok();
};

}  // namespace voicelife::linx
