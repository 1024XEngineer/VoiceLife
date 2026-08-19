#include <memory>
#include <utility>
#include <vector>

#include "support/test_support.h"
#include "voicelife/audio_esp/esp_aec_service.h"
#include "voicelife/voice/audio_frame_queue.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::audio_esp::EspAecService;
using voicelife::test::Check;
using voicelife::voice::AudioAecConfig;
using voicelife::voice::AudioAecService;
using voicelife::voice::AudioCodec;
using voicelife::voice::AudioFormat;
using voicelife::voice::AudioFrame;

namespace {

AudioAecConfig Config() {
    AudioAecConfig config;
    config.capture_format = {.codec = AudioCodec::kPcmS16Le,
                             .sample_rate_hz = 16000,
                             .channels = 1,
                             .bits_per_sample = 16,
                             .frame_duration_ms = 20};
    config.playback_reference_format = config.capture_format;
    return config;
}

// Host fake: it does not cancel echo, but models the ownership and backpressure
// contract that the ESP-SR ProcessingTask must preserve.
class FakeAecService final : public AudioAecService {
   public:
    voicelife::voice::AudioAecCapabilities capabilities() const override {
        return {.available = true, .playback_reference = true, .full_duplex = true};
    }

    Status Start(const AudioAecConfig& config, voicelife::voice::AudioFrameSink sink) override {
        const Status status = voicelife::voice::ValidateAudioAecConfig(config);
        if (!status.ok()) return status;
        if (!sink) return Status::Error(ErrorCode::kInvalidArgument, "host fake 缺少清理回调");
        config_ = config;
        queue_ = std::make_unique<voicelife::voice::BoundedAudioFrameQueue>(
            voicelife::voice::AudioQueuePolicy{.capacity_frames = config.queue_depth_frames,
                                               .full_policy = voicelife::voice::AudioQueueFullPolicy::kDropOldest});
        (void)queue_->SetGeneration(config.generation);
        sink_ = std::move(sink);
        running_ = true;
        enabled_ = false;
        return Status::Ok();
    }

    Status SubmitCapture(AudioFrame frame) override {
        if (!running_) return Status::Error(ErrorCode::kUnavailable, "host fake 未启动");
        ++stats_.capture_frames;
        const Status status = queue_->Push(std::move(frame));
        if (!status.ok()) {
            if (status.code == ErrorCode::kConflict) ++stats_.stale_generation_frames;
            return status;
        }
        const auto queue_stats = queue_->stats();
        stats_.dropped_capture_frames = queue_stats.dropped_oldest;
        Process();
        return Status::Ok();
    }

    Status SubmitPlaybackReference(AudioFrame frame) override {
        if (!running_) return Status::Error(ErrorCode::kUnavailable, "host fake 未启动");
        ++stats_.reference_frames;
        if (frame.generation != config_.generation) {
            ++stats_.stale_generation_frames;
            return Status::Error(ErrorCode::kConflict, "reference generation 已过期");
        }
        has_reference_ = true;
        Process();
        return Status::Ok();
    }

    Status SetEnabled(bool enabled) override {
        if (!running_) return Status::Error(ErrorCode::kUnavailable, "host fake 未启动");
        enabled_ = enabled;
        return Status::Ok();
    }

    Status Reset(uint64_t generation) override {
        config_.generation = generation;
        has_reference_ = false;
        return queue_ ? queue_->SetGeneration(generation) : Status::Error(ErrorCode::kUnavailable, "host fake 未启动");
    }

    Status Stop() override {
        running_ = false;
        enabled_ = false;
        has_reference_ = false;
        if (queue_) queue_->Clear();
        sink_ = {};
        return Status::Ok();
    }

    voicelife::voice::AudioAecStats stats() const override { return stats_; }

   private:
    void Process() {
        if (!enabled_ || !has_reference_) return;
        while (!queue_->empty()) {
            auto frame = queue_->Pop();
            if (!frame.ok()) return;
            ++stats_.processed_frames;
            (void)sink_(std::move(*frame.value));
        }
    }

    AudioAecConfig config_ = Config();
    std::unique_ptr<voicelife::voice::BoundedAudioFrameQueue> queue_;
    voicelife::voice::AudioFrameSink sink_;
    voicelife::voice::AudioAecStats stats_;
    bool running_ = false;
    bool enabled_ = false;
    bool has_reference_ = false;
};

AudioFrame Frame(uint64_t generation, uint64_t sequence) {
    AudioFrame frame;
    frame.generation = generation;
    frame.sequence = sequence;
    frame.format = Config().capture_format;
    frame.payload = {1, 2};
    return frame;
}

}  // namespace

int main() {
    const AudioAecConfig valid = Config();
    Check(voicelife::voice::ValidateAudioAecConfig(valid).ok(), "合法 PCM capture/reference 配置应通过校验");

    AudioAecConfig mismatched = valid;
    mismatched.playback_reference_format.sample_rate_hz = 24000;
    Check(voicelife::voice::ValidateAudioAecConfig(mismatched).code == ErrorCode::kInvalidArgument,
          "AEC 不得在服务内部隐式混用采样率");

    const auto profile = voicelife::audio_esp::SparkBotEsp32s3AudioProfile();
    EspAecService service(profile);
    const auto caps = service.capabilities();
    Check(!caps.available && !caps.playback_reference && !caps.full_duplex,
          "未验证 playback reference 的 SparkBot 不得宣称 AEC 可用");
    Check(service.Start(valid, [](AudioFrame) { return Status::Ok(); }).code == ErrorCode::kUnavailable,
          "host/当前 ESP-SR 骨架必须明确返回尚未接入，而非伪造成功");
    Check(service.SetEnabled(true).code == ErrorCode::kUnavailable, "未接入处理任务时启用必须明确失败");
    Check(service.Reset(7).ok(), "代次 reset 必须可重复调用");
    Check(service.Stop().ok(), "停止未启动 AEC 必须幂等成功");

    FakeAecService fake;
    AudioAecConfig fake_config = Config();
    fake_config.queue_depth_frames = 1;
    std::vector<uint64_t> cleaned_sequences;
    Check(fake.Start(fake_config,
                     [&cleaned_sequences](AudioFrame frame) {
                         cleaned_sequences.push_back(frame.sequence);
                         return Status::Ok();
                     })
              .ok(),
          "host fake 应接受合法配置");
    Check(fake.SetEnabled(true).ok(), "host fake 启用应成功");
    Check(fake.SubmitCapture(Frame(0, 1)).ok() && fake.SubmitCapture(Frame(0, 2)).ok(),
          "采集提交不得因实时队列满载阻塞");
    Check(fake.SubmitPlaybackReference(Frame(0, 9)).ok(), "真实 reference 到达后才处理采集帧");
    Check(cleaned_sequences.size() == 1 && cleaned_sequences.front() == 2, "AEC 满载策略必须丢旧采集帧并保留最新帧");
    Check(fake.SubmitPlaybackReference(Frame(1, 10)).code == ErrorCode::kConflict,
          "旧代次 playback reference 必须拒绝");
    const auto fake_stats = fake.stats();
    Check(fake_stats.dropped_capture_frames == 1 && fake_stats.processed_frames == 1,
          "host fake 必须暴露丢帧与处理统计");
    return 0;
}
