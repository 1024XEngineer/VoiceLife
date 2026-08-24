#include "serial_voice_protocol.h"

#include <array>

#include "support/test_support.h"

using voicelife::runtime::detail::IsValidSerialVoiceHeader;
using voicelife::runtime::detail::kSerialVoiceBegin;
using voicelife::runtime::detail::kSerialVoicePcm;
using voicelife::runtime::detail::kSerialVoicePcmBytes;
using voicelife::runtime::detail::kSerialVoiceProtocolVersion;
using voicelife::runtime::detail::kSerialVoiceWakeBegin;
using voicelife::runtime::detail::kSerialVoiceWakeEnd;
using voicelife::runtime::detail::SerialVoiceFrameHeader;
using voicelife::runtime::detail::SerialVoiceMagicMatcher;
using voicelife::test::Check;

int main() {
    SerialVoiceMagicMatcher matcher;
    constexpr std::array<uint8_t, 6> kOverlappingMagic = {'V', 'L', 'V', 'L', 'V', 'T'};
    for (std::size_t index = 0; index < kOverlappingMagic.size() - 1; ++index) {
        Check(!matcher.Push(kOverlappingMagic[index]), "重叠 magic 的前缀不能过早产生完整帧");
    }
    Check(matcher.Push(kOverlappingMagic.back()), "VLVLVT 必须在第二个重叠 VLVT 处重新同步");
    constexpr std::array<uint8_t, 4> kMagic = {'V', 'L', 'V', 'T'};
    for (std::size_t index = 0; index < kMagic.size() - 1; ++index) {
        Check(!matcher.Push(kMagic[index]), "完整匹配后必须接受下一帧 magic");
    }
    Check(matcher.Push(kMagic.back()), "连续合法帧必须分别被识别");

    Check(IsValidSerialVoiceHeader(
              {.version = kSerialVoiceProtocolVersion, .kind = kSerialVoiceBegin, .payload_bytes = 0}),
          "begin 帧必须是空 payload");
    Check(IsValidSerialVoiceHeader({.version = kSerialVoiceProtocolVersion,
                                    .kind = kSerialVoicePcm,
                                    .payload_bytes = static_cast<uint16_t>(kSerialVoicePcmBytes)}),
          "PCM 帧必须接受精确 20 ms payload");
    Check(!IsValidSerialVoiceHeader({.version = kSerialVoiceProtocolVersion,
                                     .kind = kSerialVoicePcm,
                                     .payload_bytes = static_cast<uint16_t>(kSerialVoicePcmBytes + 1)}),
          "超出固定 PCM 容量的声明长度必须在读取 payload 前拒绝");
    Check(!IsValidSerialVoiceHeader({.version = kSerialVoiceProtocolVersion, .kind = 99, .payload_bytes = 0}),
          "未知 kind 不能作为合法空帧进入状态机");
    Check(IsValidSerialVoiceHeader(
              {.version = kSerialVoiceProtocolVersion, .kind = kSerialVoiceWakeBegin, .payload_bytes = 0}),
          "wake_begin 帧必须是合法空帧");
    Check(IsValidSerialVoiceHeader(
              {.version = kSerialVoiceProtocolVersion, .kind = kSerialVoiceWakeEnd, .payload_bytes = 0}),
          "wake_end 帧必须是合法空帧");
    return 0;
}
