#pragma once

#include "voicelife/voice/voice_types.h"

namespace voicelife::voice::frame_validation {

// Shared session-internal frame contract. Sequence and session state remain
// owned by VoiceSession because they are transition-specific.
bool MatchesFormat(const AudioFrame& frame, const AudioFormat& expected);
bool MatchesSessionFrame(const AudioFrame& frame, const AudioFormat& expected, uint64_t generation);

}  // namespace voicelife::voice::frame_validation
