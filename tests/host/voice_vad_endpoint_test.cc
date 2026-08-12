#include "voice_vad_endpoint.h"

#include <chrono>
#include <cstdint>
#include <vector>

#include "support/test_support.h"

using voicelife::test::Check;

namespace {

voicelife::voice::AudioFrame PcmFrame(int16_t sample) {
    voicelife::voice::AudioFrame frame;
    frame.payload.resize(sizeof(sample));
    const auto* bytes = reinterpret_cast<const uint8_t*>(&sample);
    frame.payload.assign(bytes, bytes + sizeof(sample));
    return frame;
}

}  // namespace

int main() {
    voicelife::voice::VoiceVadEndpoint endpoint;
    const auto start = std::chrono::steady_clock::time_point(std::chrono::seconds(1));
    Check(!endpoint.Observe(PcmFrame(400), start), "首个语音帧不应触发静音端点");
    Check(!endpoint.Observe(PcmFrame(100), start + std::chrono::milliseconds(1199)), "静音窗口未到期不得触发");
    Check(endpoint.Observe(PcmFrame(100), start + std::chrono::milliseconds(1200)), "持续静音应触发一次端点");
    Check(!endpoint.Observe(PcmFrame(100), start + std::chrono::milliseconds(2400)), "同一轮静音只能触发一次");
    endpoint.Reset();
    Check(!endpoint.Observe(PcmFrame(250), start), "迟滞下限不能在未检测到语音时独立触发");
    Check(!endpoint.Observe(PcmFrame(400), start), "语音帧应建立迟滞状态");
    Check(!endpoint.Observe(PcmFrame(250), start + std::chrono::milliseconds(1300)), "迟滞范围内帧应刷新语音时间");
    return 0;
}
