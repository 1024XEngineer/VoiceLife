#include "runtime_voice_wiring.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <utility>

#include "voicelife/audio_esp/audio_board_profile.h"

namespace voicelife::runtime {

void VoiceLifePcbVoiceWiring::Assemble(voice::SpeechProviderAdapter& provider, voice::EvidenceSink evidence_sink,
                                       voice::WakeGateAudioInput::WakeSink wake_sink, int volume) {
    audio_ports_ = std::make_unique<audio_esp::Esp32s3PcmAudioPorts>(audio_esp::VoiceLifePcbEsp32s3Profile());
    SetOutputVolume(volume);
    wake_detector_ = std::make_unique<audio_esp::EspMultiNetWakeDetector>();
    wake_gate_ = std::make_unique<voice::WakeGateAudioInput>(audio_ports_->input(), *wake_detector_);
    wake_gate_->SetWakeSink(std::move(wake_sink));
    session_ =
        std::make_unique<voice::VoiceSession>(*wake_gate_, audio_ports_->output(), provider, std::move(evidence_sink));
}

Status VoiceLifePcbVoiceWiring::StartSession() {
    if (session_ == nullptr) return Status::Error(ErrorCode::kUnavailable, "旧板语音会话尚未装配");
    voice::VoiceSessionConfig config;
    config.session_id = "voicelife-linx-session";
    config.provider_id = "xrobot-websocket";
    config.mode = voice::VoiceMode::kRealtime;
    config.audio.codec = voice::AudioCodec::kPcmS16Le;
    config.audio.sample_rate_hz = 16000;
    config.audio.channels = 1;
    config.audio.bits_per_sample = 16;
    config.audio.frame_duration_ms = 20;
    return session_->Start(config);
}

audio_esp::AudioPortStats VoiceLifePcbVoiceWiring::audio_stats() const {
    return audio_ports_ ? audio_ports_->stats() : audio_esp::AudioPortStats{};
}

void VoiceLifePcbVoiceWiring::SetOutputVolume(int volume) {
    if (audio_ports_ == nullptr) return;
    audio_ports_->SetOutputVolume(static_cast<uint8_t>(std::clamp(volume, 0, 100)));
}

}  // namespace voicelife::runtime

#endif
