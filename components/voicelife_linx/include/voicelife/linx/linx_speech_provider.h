#pragma once

#include <cstdint>
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
    [[nodiscard]] const voice::CapabilityProfile& capabilities() const override {
        return capabilities_;
    }

    static voice::CapabilityProfile DefaultCapabilities();

   private:
    void OnText(std::string_view message);
    void OnBinary(const std::vector<uint8_t>& payload);
    Status Send(Result<std::string> encoded);
    void Emit(voice::VoiceEvent event);

    LinxTransportPort& transport_;
    LinxProtocolCodecPort& codec_;
    LinxConnectionConfig connection_;
    voice::CapabilityProfile capabilities_;
    voice::VoiceSessionConfig config_;
    voice::VoiceEventSink event_sink_;
    voice::AudioFrameSink audio_sink_;
    uint64_t generation_ = 0;
    uint64_t output_sequence_ = 0;
    bool connected_ = false;
};

}  // namespace voicelife::linx
