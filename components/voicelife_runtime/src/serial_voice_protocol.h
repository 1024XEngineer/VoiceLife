#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace voicelife::runtime::detail {

inline constexpr std::array<uint8_t, 4> kSerialVoiceMagic = {'V', 'L', 'V', 'T'};
inline constexpr uint8_t kSerialVoiceProtocolVersion = 1;
inline constexpr uint8_t kSerialVoiceBegin = 1;
inline constexpr uint8_t kSerialVoicePcm = 2;
inline constexpr uint8_t kSerialVoiceEnd = 3;
// Test-only frames for feeding the local wake detector while the board stays
// in standby. They must never be interpreted as interaction press events.
inline constexpr uint8_t kSerialVoiceWakeBegin = 4;
inline constexpr uint8_t kSerialVoiceWakeEnd = 5;
inline constexpr std::size_t kSerialVoicePcmBytes = 16000U * 20U / 1000U * sizeof(int16_t);

struct SerialVoiceFrameHeader {
    uint8_t version = 0;
    uint8_t kind = 0;
    uint16_t payload_bytes = 0;
};

/** Incremental matcher that keeps valid overlapping VLVT prefixes. */
class SerialVoiceMagicMatcher final {
   public:
    [[nodiscard]] bool Push(uint8_t byte) {
        while (matched_ != 0 && byte != kSerialVoiceMagic[matched_]) {
            matched_ = kFailure[matched_ - 1];
        }
        if (byte == kSerialVoiceMagic[matched_]) {
            ++matched_;
        }
        if (matched_ != kSerialVoiceMagic.size()) {
            return false;
        }
        matched_ = kFailure.back();
        return true;
    }

   private:
    // KMP prefix lengths for V, VL, VLV, VLVT. In particular, VLVL keeps VL.
    static constexpr std::array<std::size_t, 4> kFailure = {0, 0, 1, 0};
    std::size_t matched_ = 0;
};

[[nodiscard]] inline bool IsValidSerialVoiceHeader(const SerialVoiceFrameHeader& header) {
    if (header.version != kSerialVoiceProtocolVersion || header.payload_bytes > kSerialVoicePcmBytes) {
        return false;
    }
    if (header.kind == kSerialVoicePcm) {
        return header.payload_bytes == kSerialVoicePcmBytes;
    }
    return (header.kind == kSerialVoiceBegin || header.kind == kSerialVoiceEnd ||
            header.kind == kSerialVoiceWakeBegin || header.kind == kSerialVoiceWakeEnd) &&
           header.payload_bytes == 0;
}

}  // namespace voicelife::runtime::detail
