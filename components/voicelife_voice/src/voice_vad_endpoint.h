#pragma once

#include <chrono>

#include "voicelife/voice/voice_types.h"

namespace voicelife::voice {

// Lightweight local endpoint detector for boards without an AFE. It only
// identifies the first sustained silence after speech; VoiceSession owns the
// resulting event and all capture state transitions.
class VoiceVadEndpoint {
   public:
    void Reset();
    [[nodiscard]] bool Observe(const AudioFrame& frame, std::chrono::steady_clock::time_point now);

   private:
    bool speech_seen_ = false;
    bool silence_emitted_ = false;
    std::chrono::steady_clock::time_point last_speech_at_{};
};

}  // namespace voicelife::voice
