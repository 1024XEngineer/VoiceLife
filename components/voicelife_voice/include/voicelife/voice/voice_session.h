#pragma once

#include <cstdint>

#include "voicelife/voice/voice_ports.h"

namespace voicelife::voice {

class VoiceSession {
   public:
    VoiceSession(AudioInputPort& input, AudioOutputPort& output, SpeechProviderAdapter& provider,
                 EvidenceSink evidence = {});

    Status Start(const VoiceSessionConfig& config);
    Status BeginCapture();
    Status EndCapture();
    Status SubmitAudio(AudioFrame frame);
    Status HandleAudio(AudioFrame frame);
    Status Speak(std::string_view text);
    Status Interrupt();
    Status Stop();

    [[nodiscard]] VoiceSessionState state() const { return state_; }
    [[nodiscard]] uint64_t generation() const { return generation_; }
    [[nodiscard]] const VoiceSessionConfig& config() const { return config_; }

   private:
    void Emit(std::string_view event, std::string_view detail);
    bool AcceptFrame(const AudioFrame& frame) const;

    AudioInputPort& input_;
    AudioOutputPort& output_;
    SpeechProviderAdapter& provider_;
    EvidenceSink evidence_;
    VoiceSessionConfig config_;
    VoiceSessionState state_ = VoiceSessionState::kStopped;
    uint64_t generation_ = 0;
    uint64_t next_sequence_ = 0;
};

}  // namespace voicelife::voice
