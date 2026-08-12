#include "voice_vad_endpoint.h"

#include <cstdint>

namespace voicelife::voice {

void VoiceVadEndpoint::Reset() {
    speech_seen_ = false;
    silence_emitted_ = false;
    last_speech_at_ = {};
}

bool VoiceVadEndpoint::Observe(const AudioFrame& frame, std::chrono::steady_clock::time_point now) {
    const auto* pcm = reinterpret_cast<const int16_t*>(frame.payload.data());
    const std::size_t sample_count = frame.payload.size() / sizeof(int16_t);
    int64_t energy = 0;
    for (std::size_t index = 0; index < sample_count; ++index) {
        const int64_t sample = pcm[index];
        energy += sample * sample;
    }
    const int64_t rms = sample_count > 0 ? energy / static_cast<int64_t>(sample_count) : 0;
    constexpr int64_t kSpeechEnterThreshold = 300 * 300;  // 进入语音约 -40 dBFS
    constexpr int64_t kSpeechExitThreshold = 180 * 180;   // 迟滞下限约 -44 dBFS
    if (rms >= kSpeechEnterThreshold || (speech_seen_ && rms >= kSpeechExitThreshold)) {
        speech_seen_ = true;
        last_speech_at_ = now;
        return false;
    }
    if (speech_seen_ && !silence_emitted_ && last_speech_at_.time_since_epoch().count() > 0 &&
        now - last_speech_at_ >= std::chrono::milliseconds(1200)) {
        silence_emitted_ = true;
        return true;
    }
    return false;
}

}  // namespace voicelife::voice
