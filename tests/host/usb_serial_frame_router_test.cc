#include "usb_serial_frame_router.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "serial_voice_protocol.h"
#include "support/test_support.h"

using voicelife::runtime::UsbSerialFrame;
using voicelife::runtime::UsbSerialFrameDecoder;
using voicelife::runtime::UsbSerialFrameKind;
using voicelife::test::Check;

namespace {

std::vector<uint8_t> ImFrame(std::string_view magic, std::string_view origin, std::string_view device_id,
                             std::string_view token, std::string_view user_id) {
    std::vector<uint8_t> frame(magic.begin(), magic.end());
    for (const std::string_view value : {origin, device_id, token, user_id}) {
        frame.push_back(static_cast<uint8_t>(value.size() >> 8U));
        frame.push_back(static_cast<uint8_t>(value.size()));
    }
    for (const std::string_view value : {origin, device_id, token, user_id})
        frame.insert(frame.end(), value.begin(), value.end());
    return frame;
}

std::vector<uint8_t> VoiceFrame(uint8_t kind, std::span<const uint8_t> payload = {}) {
    std::vector<uint8_t> frame{
        'V', 'L', 'V', 'T', 1, kind, static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8U)};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<UsbSerialFrame> Push(UsbSerialFrameDecoder& decoder, std::span<const uint8_t> bytes) {
    std::vector<UsbSerialFrame> frames;
    for (const uint8_t byte : bytes) {
        if (auto frame = decoder.Push(byte); frame.has_value()) frames.push_back(*frame);
    }
    return frames;
}

}  // namespace

int main() {
    UsbSerialFrameDecoder decoder;
    const auto provisioning = ImFrame("VLI1", "https://gateway.test", "device-test", "token-test", "user-test");
    const auto voice_begin = VoiceFrame(voicelife::runtime::detail::kSerialVoiceBegin);
    constexpr std::array<uint8_t, 12> kPairing = {'V', 'L', 'P', '1', 5, 0, 0, 0, 0, 0, 0, 0};

    const auto prefix = Push(decoder, std::span<const uint8_t>(provisioning.data(), 7));
    Check(prefix.empty(), "不完整的 VLI1 固定头不能过早路由到 IM");
    const auto rest = Push(decoder, std::span<const uint8_t>(provisioning.data() + 7, provisioning.size() - 7));
    Check(rest.size() == 1 && rest.front().kind == UsbSerialFrameKind::kImProvisioning,
          "完整 VLI1 必须仅路由到 IM provisioning 队列");
    Check(rest.front().view().size() == provisioning.size(), "IM 路由必须保留完整帧边界");

    const auto voice = Push(decoder, voice_begin);
    Check(voice.size() == 1 && voice.front().kind == UsbSerialFrameKind::kSerialVoice,
          "VLVT begin 必须只路由到语音队列");
    const auto pairing = Push(decoder, kPairing);
    Check(pairing.size() == 1 && pairing.front().kind == UsbSerialFrameKind::kImPairing,
          "VLP1 必须只路由到 IM 配对队列");

    UsbSerialFrameDecoder interleaved_decoder;
    std::vector<uint8_t> pcm(voicelife::runtime::detail::kSerialVoicePcmBytes, 0);
    pcm[128] = 'V';
    pcm[129] = 'L';
    pcm[130] = 'I';
    pcm[131] = '1';
    const auto pcm_frame = VoiceFrame(voicelife::runtime::detail::kSerialVoicePcm, pcm);
    const auto pcm_result = Push(interleaved_decoder, pcm_frame);
    Check(pcm_result.size() == 1 && pcm_result.front().kind == UsbSerialFrameKind::kSerialVoice,
          "语音 payload 内的 VLI1 前缀不能被错误交给 IM");

    UsbSerialFrameDecoder malformed_decoder;
    constexpr std::array<uint8_t, 12> kBadPairing = {'V', 'L', 'P', '1', 0, 0, 0, 0, 0, 0, 0, 0};
    Check(Push(malformed_decoder, kBadPairing).empty(), "非法 VLP1 参数必须在路由层丢弃");
    const auto recovered = Push(malformed_decoder, voice_begin);
    Check(recovered.size() == 1 && recovered.front().kind == UsbSerialFrameKind::kSerialVoice,
          "非法帧之后必须重新同步，不影响下一条合法 VLVT");
    return 0;
}
