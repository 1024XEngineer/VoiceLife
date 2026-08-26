#include <atomic>
#include <cstdint>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "support/test_support.h"
#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#include "voicelife/audio_esp/pcm_frame_assembler.h"
#include "voicelife/audio_esp/sparkbot_audio_budget.h"

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
    Check(assembler.Prepare().ok(), "合法 PCM 组帧器必须能在采集前准备缓存");
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

    PcmFrameAssembler segmented(Pcm(60), 10);
    Check(segmented.Prepare().ok(), "分段 PCM 组帧器必须能在采集前准备缓存");
    std::vector<voicelife::voice::AudioFrame> segmented_frames;
    const PcmFrameAssembler::Sink segmented_sink = [&segmented_frames](voicelife::voice::AudioFrame frame) {
        segmented_frames.push_back(std::move(frame));
        return Status::Ok();
    };
    std::vector<std::int16_t> sequential(1920);
    for (std::size_t i = 0; i < sequential.size(); ++i) {
        sequential[i] = static_cast<std::int16_t>(i);
    }
    Check(segmented.Push(sequential.data(), 400, segmented_sink).ok(), "首段样本应暂存");
    Check(segmented.Push(sequential.data() + 400, 800, segmented_sink).ok(), "跨帧第二段应完成首帧");
    Check(segmented.pending_samples() == 240, "跨帧输入后的尾部样本必须保留");
    Check(segmented.Push(sequential.data() + 1200, 720, segmented_sink).ok(), "第三段应完成第二帧");
    Check(segmented_frames.size() == 2 && segmented.pending_samples() == 0, "分段输入必须产生两帧且不残留");
    std::int16_t first_sample = 0;
    std::int16_t last_first_frame_sample = 0;
    std::int16_t first_second_frame_sample = 0;
    std::memcpy(&first_sample, segmented_frames[0].payload.data(), sizeof(first_sample));
    std::memcpy(&last_first_frame_sample, segmented_frames[0].payload.data() + 959 * sizeof(std::int16_t),
                sizeof(last_first_frame_sample));
    std::memcpy(&first_second_frame_sample, segmented_frames[1].payload.data(), sizeof(first_second_frame_sample));
    Check(first_sample == 0 && last_first_frame_sample == 959 && first_second_frame_sample == 960,
          "分段组帧不能改变 PCM 样本顺序");

    PcmFrameAssembler pooled(Pcm(20), 10);
    Check(pooled.Prepare().ok(), "实时 PCM 组帧器必须在采集前建立固定 payload pool");
    std::vector<voicelife::voice::AudioFrame> retained_frames;
    const PcmFrameAssembler::Sink retain_sink = [&retained_frames](voicelife::voice::AudioFrame frame) {
        retained_frames.push_back(std::move(frame));
        return Status::Ok();
    };
    for (int index = 0; index < 16; ++index) {
        Check(pooled.Push(period.data(), period.size(), retain_sink).ok(), "固定 pool 容量内首个 period 必须被暂存");
        Check(pooled.Push(period.data(), period.size(), retain_sink).ok(), "固定 pool 容量内必须交付完整 PCM 帧");
    }
    Check(pooled.payload_pool_high_watermark() == 16, "pool 高水位必须反映全部在途 PCM lease");
    Check(pooled.Push(period.data(), period.size(), retain_sink).ok(), "耗尽前的首个 period 必须仍可被组装");
    Check(pooled.Push(period.data(), period.size(), retain_sink).code == ErrorCode::kUnavailable,
          "pool 耗尽时采集路径必须立即失败，不能退回堆分配或等待消费者");
    Check(pooled.payload_pool_acquisition_failures() == 1, "pool 耗尽必须留下可观测计数");
    retained_frames.erase(retained_frames.begin());
    Check(pooled.Push(period.data(), period.size(), retain_sink).ok(), "释放后的首个 period 必须被组装");
    Check(pooled.Push(period.data(), period.size(), retain_sink).ok(),
          "异步消费者释放 lease 后必须能再次取得固定 pool slot");

    auto concurrent_pool = voicelife::voice::AudioPayloadPool::Create(16, 640);
    Check(concurrent_pool != nullptr, "实时 PCM pool 必须支持 16 个原子租约");
    std::atomic_bool start{false};
    std::atomic_uint64_t acquisition_failures{0};
    const auto exercise_pool = [&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int index = 0; index < 10000; ++index) {
            auto lease = concurrent_pool->TryAcquire();
            if (!lease.pooled()) {
                ++acquisition_failures;
                continue;
            }
            lease.resize(640);
        }
    };
    std::thread first(exercise_pool);
    std::thread second(exercise_pool);
    start.store(true, std::memory_order_release);
    first.join();
    second.join();
    Check(acquisition_failures.load() == 0, "有空闲 slot 时跨线程获取和归还不能因短暂竞争被误判为 pool 耗尽");
    Check(concurrent_pool->acquisition_failures() == 0, "原子 pool 的失败统计只能表示真实容量耗尽，不能包含锁竞争");
    Check(concurrent_pool->high_watermark() <= 2, "并发租约高水位不能超过同时在途的两个消费者");
    constexpr std::size_t kDecodePoolSlots = voicelife::audio_esp::kSparkBotOpusDecodePoolSlots;
    static_assert(kDecodePoolSlots >= voicelife::audio_esp::kSparkBotPlaybackQueueDepth + 2);
    auto decode_pool = voicelife::voice::AudioPayloadPool::Create(kDecodePoolSlots, 640);
    Check(decode_pool != nullptr, "SparkBot Opus 下行池必须覆盖播放队列和在途帧");
    std::vector<voicelife::voice::AudioPayload> decode_leases;
    decode_leases.reserve(kDecodePoolSlots);
    std::set<const uint8_t*> decode_addresses;
    for (std::size_t index = 0; index < kDecodePoolSlots; ++index) {
        auto payload = decode_pool->TryAcquire();
        Check(payload.pooled(), "下行池容量内必须返回池化租约");
        decode_addresses.insert(payload.data());
        decode_leases.push_back(std::move(payload));
    }
    Check(decode_addresses.size() == kDecodePoolSlots, "下行池每个槽位必须拥有独立地址");
    Check(decode_pool->in_use() == kDecodePoolSlots, "保留的下行帧必须反映为全部槽位在用");
    Check(decode_pool->high_watermark() == kDecodePoolSlots, "下行池高水位必须覆盖播放容量契约");
    Check(!decode_pool->TryAcquire().pooled(), "下行池耗尽时必须立即拒绝第 99 个租约");
    Check(decode_pool->acquisition_failures() == 1, "下行池耗尽必须留下准确失败计数");
    decode_leases.pop_back();
    Check(decode_pool->in_use() == kDecodePoolSlots - 1, "释放一个下行帧后在用数必须下降");
    Check(decode_pool->TryAcquire().pooled(), "释放后的下行槽位必须立即可重用");

    Check(voicelife::voice::AudioPayloadPool::Create(voicelife::voice::AudioPayloadPool::kMaximumSlots + 1, 640) ==
              nullptr,
          "原子位图实现必须拒绝超过最大槽位数的配置");

    Check(assembler.Push(nullptr, 1, sink).code == ErrorCode::kInvalidArgument, "非零样本数不能搭配空指针");
    Check(assembler.Push(period.data(), period.size(), {}).code == ErrorCode::kInvalidArgument, "组帧必须拒绝空 sink");

    PcmFrameAssembler partial(Pcm(60), 10);
    Check(partial.Prepare().ok(), "部分 PCM 组帧器必须能在采集前准备缓存");
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
    Check(stereo.Prepare().ok(), "双声道 PCM 组帧器必须能在采集前准备缓存");
    Check(stereo.Push(period.data(), 161, sink).code == ErrorCode::kInvalidArgument,
          "双声道组帧不能接受非整声道样本数");

    auto oversized = Pcm(1000);
    oversized.sample_rate_hz = UINT32_MAX;
    PcmFrameAssembler oversized_assembler(oversized, 10);
    Check(oversized_assembler.Validate().code == ErrorCode::kInvalidArgument,
          "超过 AudioFrame 负载上限的远端协商格式必须在分配前拒绝");
    Check(oversized_assembler.Prepare().code == ErrorCode::kInvalidArgument, "超大协商格式不能触发组帧缓存分配");

    const auto profile = voicelife::audio_esp::VoiceLifePcbEsp32s3Profile();
    Esp32s3PcmAudioPorts ports(profile);
    auto capture = Pcm(60);
    capture.sample_rate_hz = 16000;
    auto playback = Pcm(60);
    playback.sample_rate_hz = 24000;
    Check(ports.input().Open(capture).code == ErrorCode::kUnavailable, "主机 Audio Port 不能伪造 ESP32-S3 采集已打开");
    Check(ports.output().Open(playback).code == ErrorCode::kUnavailable,
          "主机 Audio Port 不能伪造 ESP32-S3 播放已打开");
    auto oversized_playback = playback;
    oversized_playback.channels = 2;
    oversized_playback.frame_duration_ms = UINT16_MAX;
    Check(ports.output().Open(oversized_playback).code == ErrorCode::kInvalidArgument,
          "超过 AudioFrame 负载上限的下行协商格式必须在 scratch 分配前拒绝");
    Check(ports.input().StartCapture(voicelife::voice::VoiceMode::kManual).code == ErrorCode::kUnavailable,
          "主机 Audio Port 不能伪造 ESP32-S3 采集已启动");

    return 0;
}
