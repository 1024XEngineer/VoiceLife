#include "voicelife/audio_esp/pcm_frame_assembler.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace voicelife::audio_esp {
namespace {

Status Invalid(std::string message) { return Status::Error(ErrorCode::kInvalidArgument, std::move(message)); }

}  // namespace

PcmFrameAssembler::PcmFrameAssembler(voice::AudioFormat frame_format, uint16_t hardware_period_ms)
    : frame_format_(std::move(frame_format)), hardware_period_ms_(hardware_period_ms) {
    const uint64_t frame_numerator =
        static_cast<uint64_t>(frame_format_.sample_rate_hz) * frame_format_.frame_duration_ms;
    if (frame_format_.channels != 0 && frame_numerator % 1000 == 0) {
        const uint64_t samples_per_channel = frame_numerator / 1000;
        if (samples_per_channel <= std::numeric_limits<std::size_t>::max() / frame_format_.channels) {
            frame_samples_ = static_cast<std::size_t>(samples_per_channel) * frame_format_.channels;
        }
    }
}

PcmFrameAssembler::~PcmFrameAssembler() {
#ifdef ESP_PLATFORM
    heap_caps_free(pending_samples_);
#else
    delete[] pending_samples_;
#endif
}

Status PcmFrameAssembler::Validate() const {
    if (!frame_format_.valid() || frame_format_.codec != voice::AudioCodec::kPcmS16Le ||
        frame_format_.bits_per_sample != 16 || frame_format_.channels == 0 || hardware_period_ms_ == 0) {
        return Invalid("PCM 组帧格式必须是合法的 S16LE");
    }
    const uint64_t period_numerator = static_cast<uint64_t>(frame_format_.sample_rate_hz) * hardware_period_ms_;
    const uint64_t frame_numerator =
        static_cast<uint64_t>(frame_format_.sample_rate_hz) * frame_format_.frame_duration_ms;
    const uint64_t samples_per_channel = frame_numerator / 1000;
    if (period_numerator % 1000 != 0 || frame_numerator % 1000 != 0 ||
        frame_format_.frame_duration_ms % hardware_period_ms_ != 0 || samples_per_channel == 0 ||
        samples_per_channel > std::numeric_limits<std::size_t>::max() / frame_format_.channels || frame_samples_ == 0 ||
        frame_samples_ > voice::AudioFrame::kMaxPayloadBytes / sizeof(int16_t)) {
        return Invalid("传输帧时长必须是硬件 period 的整数倍");
    }
    return Status::Ok();
}

Status PcmFrameAssembler::Prepare() {
    const Status validation = Validate();
    if (!validation.ok()) {
        return validation;
    }
    if (prepared_) {
        return Status::Ok();
    }
#ifdef ESP_PLATFORM
    pending_samples_ =
        static_cast<int16_t*>(heap_caps_malloc(frame_samples_ * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    pending_samples_ = new (std::nothrow) int16_t[frame_samples_];
#endif
    if (pending_samples_ == nullptr) return Status::Error(ErrorCode::kUnavailable, "PCM 组帧缓存分配失败");
    prepared_ = true;
    return Status::Ok();
}

Status PcmFrameAssembler::Push(const int16_t* samples, std::size_t sample_count, const Sink& sink) {
    if (!prepared_) {
        return Status::Error(ErrorCode::kUnavailable, "PCM 组帧器尚未准备");
    }
    if (sample_count != 0 && samples == nullptr) {
        return Invalid("PCM 组帧不能接收空样本指针");
    }
    if (!sink) {
        return Invalid("PCM 组帧缺少输出 sink");
    }

    const std::size_t channel_count = frame_format_.channels;
    if (frame_samples_ == 0 || sample_count % channel_count != 0) {
        return Invalid("PCM 样本数不能组成完整声道帧");
    }
    while (sample_count != 0) {
        const std::size_t copied = std::min(sample_count, frame_samples_ - pending_size_);
        std::memcpy(pending_samples_ + pending_size_, samples, copied * sizeof(int16_t));
        pending_size_ += copied;
        samples += copied;
        sample_count -= copied;
        if (pending_size_ != frame_samples_) continue;
        voice::AudioFrame frame;
        frame.format = frame_format_;
        if (frame_samples_ > std::numeric_limits<std::size_t>::max() / sizeof(int16_t)) {
            return Invalid("PCM 组帧负载长度溢出");
        }
        frame.payload.resize(frame_samples_ * sizeof(int16_t));
        std::memcpy(frame.payload.data(), pending_samples_, frame.payload.size());
        pending_size_ = 0;
        const Status status = sink(std::move(frame));
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

void PcmFrameAssembler::Reset() { pending_size_ = 0; }

}  // namespace voicelife::audio_esp
