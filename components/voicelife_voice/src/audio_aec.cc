#include "voicelife/voice/audio_aec.h"

#include <utility>

namespace voicelife::voice {

Status ValidateAudioAecConfig(const AudioAecConfig& config) {
    const auto valid_pcm = [](const AudioFormat& format) {
        return format.valid() && format.codec == AudioCodec::kPcmS16Le && format.bits_per_sample == 16;
    };
    if (!valid_pcm(config.capture_format) || !valid_pcm(config.playback_reference_format)) {
        return Status::Error(ErrorCode::kInvalidArgument, "AEC 只接受合法的 PCM S16LE 格式");
    }
    if (config.capture_format.sample_rate_hz != config.playback_reference_format.sample_rate_hz ||
        config.capture_format.frame_duration_ms != config.playback_reference_format.frame_duration_ms) {
        return Status::Error(ErrorCode::kInvalidArgument, "AEC capture/reference 格式必须采样率和帧长一致");
    }
    if (config.tail_length_ms == 0 || config.tail_length_ms > 2000) {
        return Status::Error(ErrorCode::kInvalidArgument, "AEC 回声尾长必须在 1 到 2000 ms 之间");
    }
    if (config.queue_depth_frames == 0 || config.queue_depth_frames > 32) {
        return Status::Error(ErrorCode::kInvalidArgument, "AEC 实时队列深度必须在 1 到 32 帧之间");
    }
    return Status::Ok();
}

}  // namespace voicelife::voice
