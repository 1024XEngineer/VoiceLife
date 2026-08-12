#include "voice_frame_validation.h"

namespace voicelife::voice::frame_validation {

bool MatchesFormat(const AudioFrame& frame, const AudioFormat& expected) {
    const AudioFormat& actual = frame.format;
    return actual.valid() && actual.codec == expected.codec && actual.sample_rate_hz == expected.sample_rate_hz &&
           actual.channels == expected.channels && actual.bits_per_sample == expected.bits_per_sample &&
           actual.frame_duration_ms == expected.frame_duration_ms && !frame.payload.empty() &&
           frame.payload.size() <= AudioFrame::kMaxPayloadBytes;
}

bool MatchesSessionFrame(const AudioFrame& frame, const AudioFormat& expected, uint64_t generation) {
    return frame.generation == generation && MatchesFormat(frame, expected);
}

}  // namespace voicelife::voice::frame_validation
