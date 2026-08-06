#include <cstdint>
#include <vector>

#include "support/test_support.h"
#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#include "voicelife/audio_esp/pcm_frame_assembler.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;

namespace {

voicelife::voice::AudioFormat Pcm(std::uint16_t duration_ms) {
    return {.codec = voicelife::voice::AudioCodec::kPcmS16Le,
            .sample_rate_hz = 16000,
            .channels = 1,
            .bits_per_sample = 16,
            .frame_duration_ms = duration_ms};
}

}  // namespace

int main() {
    using voicelife::audio_esp::Esp32s3PcmAudioPorts;
    using voicelife::audio_esp::PcmFrameAssembler;

    PcmFrameAssembler assembler(Pcm(60), 10);
    Check(assembler.Validate().ok(), "60 ms 传输帧应能由 10 ms 硬件 period 组装");
    Check(assembler.frame_samples() == 960, "16 kHz 单声道 60 ms 应包含 960 个样本");

    std::vector<voicelife::voice::AudioFrame> frames;
    const PcmFrameAssembler::Sink sink = [&frames](voicelife::voice::AudioFrame frame) {
        frames.push_back(std::move(frame));
        return Status::Ok();
    };
    std::vector<std::int16_t> period(160, 7);
    for (int i = 0; i < 5; ++i) {
        Check(assembler.Push(period.data(), period.size(), sink).ok(), "完整硬件 period 应能进入组帧缓存");
        Check(frames.empty(), "不足一个传输帧时不能提前向上层投递");
    }
    Check(assembler.Push(period.data(), period.size(), sink).ok(), "第六个 period 应完成组帧");
    Check(frames.size() == 1 && frames.front().payload.size() == 960U * sizeof(std::int16_t),
          "60 ms PCM 负载字节数必须准确");
    Check(frames.front().format.frame_duration_ms == 60, "组帧不能改变协商帧时长");
    Check(assembler.pending_samples() == 0, "完整帧投递后不能残留样本");

    Check(assembler.Push(nullptr, 1, sink).code == ErrorCode::kInvalidArgument, "非零样本数不能搭配空指针");
    Check(assembler.Push(period.data(), period.size(), {}).code == ErrorCode::kInvalidArgument, "组帧必须拒绝空 sink");

    PcmFrameAssembler partial(Pcm(60), 10);
    Check(partial.Push(period.data(), 80, sink).ok(), "半帧样本应暂存");
    Check(partial.pending_samples() == 80, "半帧样本必须保留在缓存中");
    partial.Reset();
    Check(partial.pending_samples() == 0, "Reset 必须清理半帧缓存");

    PcmFrameAssembler invalid_duration(Pcm(15), 10);
    Check(invalid_duration.Validate().code == ErrorCode::kInvalidArgument,
          "不能整除硬件 period 的 15 ms 传输帧必须拒绝");

    auto invalid_samples = Pcm(60);
    invalid_samples.channels = 2;
    PcmFrameAssembler stereo(invalid_samples, 10);
    Check(stereo.Validate().ok(), "双声道 PCM 组帧格式应合法");
    Check(stereo.Push(period.data(), 161, sink).code == ErrorCode::kInvalidArgument,
          "双声道组帧不能接受非整声道样本数");

    const auto profile = voicelife::audio_esp::VoiceLifePcbEsp32s3Profile();
    Esp32s3PcmAudioPorts ports(profile);
    auto capture = Pcm(60);
    capture.sample_rate_hz = 16000;
    auto playback = Pcm(60);
    playback.sample_rate_hz = 24000;
    Check(ports.input().Open(capture).code == ErrorCode::kUnavailable, "主机 Audio Port 不能伪造 ESP32-S3 采集已打开");
    Check(ports.output().Open(playback).code == ErrorCode::kUnavailable,
          "主机 Audio Port 不能伪造 ESP32-S3 播放已打开");
    Check(ports.input().StartCapture(voicelife::voice::VoiceMode::kManual).code == ErrorCode::kUnavailable,
          "主机 Audio Port 不能伪造 ESP32-S3 采集已启动");

    return 0;
}
