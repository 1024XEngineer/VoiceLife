#include "voice_frame_validation.h"

#include <cstdint>

#include "support/test_support.h"

using voicelife::test::Check;
namespace frame_validation = voicelife::voice::frame_validation;

namespace {

voicelife::voice::AudioFrame ValidFrame() {
    voicelife::voice::AudioFrame frame;
    frame.generation = 7;
    frame.payload = {1, 2, 3, 4};
    return frame;
}

}  // namespace

int main() {
    const auto expected = ValidFrame().format;
    Check(frame_validation::MatchesFormat(ValidFrame(), expected), "完整匹配的非空帧必须通过格式校验");
    Check(frame_validation::MatchesSessionFrame(ValidFrame(), expected, 7), "当前 generation 的采集帧必须通过");
    Check(!frame_validation::MatchesSessionFrame(ValidFrame(), expected, 8), "旧 generation 采集帧必须拒绝");

    auto wrong_rate = ValidFrame();
    wrong_rate.format.sample_rate_hz = 8000;
    Check(!frame_validation::MatchesFormat(wrong_rate, expected), "采样率变化必须拒绝");
    auto empty_payload = ValidFrame();
    empty_payload.payload.clear();
    Check(!frame_validation::MatchesFormat(empty_payload, expected), "空载荷必须拒绝");
    auto oversized_payload = ValidFrame();
    oversized_payload.payload.resize(voicelife::voice::AudioFrame::kMaxPayloadBytes + 1);
    Check(!frame_validation::MatchesFormat(oversized_payload, expected), "超出单帧预算的载荷必须拒绝");
    return 0;
}
