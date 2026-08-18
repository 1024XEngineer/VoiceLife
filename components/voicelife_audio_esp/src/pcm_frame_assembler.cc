#include "voicelife/audio_esp/pcm_frame_assembler.h"

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

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
    try {
        // 组帧器在端口打开时创建并准备，避免首个 I2S period 在实时采集任务扩容。
        pending_samples_.reserve(frame_samples_);
    } catch (const std::bad_alloc&) {
        return Status::Error(ErrorCode::kUnavailable, "PCM 组帧缓存分配失败");
    } catch (const std::length_error&) {
        return Invalid("PCM 组帧缓存长度超出限制");
    }
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
    const std::size_t pending_count = pending_samples_.size() - pending_offset_;
    if (sample_count > std::numeric_limits<std::size_t>::max() - pending_count) {
        return Invalid("PCM 组帧缓存长度溢出");
    }
    // Capture normally supplies fixed hardware periods. Keep a read offset so
    // completing a transmission frame does not shift its full payload in the
    // realtime capture task. Compact only when a partial tail needs room.
    if (pending_offset_ == pending_samples_.size()) {
        pending_samples_.clear();
        pending_offset_ = 0;
    } else if (pending_samples_.capacity() - pending_samples_.size() < sample_count) {
        if (pending_count != 0) {
            std::memmove(pending_samples_.data(), pending_samples_.data() + pending_offset_,
                         pending_count * sizeof(int16_t));
        }
        pending_samples_.resize(pending_count);
        pending_offset_ = 0;
    }
    if (sample_count != 0) {
        pending_samples_.insert(pending_samples_.end(), samples, samples + sample_count);
    }

    while (pending_samples_.size() - pending_offset_ >= frame_samples_) {
        voice::AudioFrame frame;
        frame.format = frame_format_;
        if (frame_samples_ > std::numeric_limits<std::size_t>::max() / sizeof(int16_t)) {
            return Invalid("PCM 组帧负载长度溢出");
        }
        frame.payload.resize(frame_samples_ * sizeof(int16_t));
        std::memcpy(frame.payload.data(), pending_samples_.data() + pending_offset_, frame.payload.size());
        pending_offset_ += frame_samples_;
        const Status status = sink(std::move(frame));
        if (!status.ok()) {
            return status;
        }
    }
    if (pending_offset_ == pending_samples_.size()) {
        pending_samples_.clear();
        pending_offset_ = 0;
    }
    return Status::Ok();
}

void PcmFrameAssembler::Reset() {
    pending_samples_.clear();
    pending_offset_ = 0;
}

}  // namespace voicelife::audio_esp
