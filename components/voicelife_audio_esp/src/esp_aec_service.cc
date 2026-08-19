#include "voicelife/audio_esp/esp_aec_service.h"

#include <utility>

namespace voicelife::audio_esp {

namespace {

Status Unavailable(std::string message) { return Status::Error(ErrorCode::kUnavailable, std::move(message)); }

}  // namespace

EspAecService::EspAecService(AudioBoardProfile profile) : profile_(std::move(profile)) {}

voice::AudioAecCapabilities EspAecService::capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool reference = profile_.input_reference;
    const bool duplex = reference && profile_.topology == AudioBoardTopology::kExternalCodecDuplex;
    return {.available = false, .playback_reference = reference, .full_duplex = duplex};
}

Status EspAecService::Start(const voice::AudioAecConfig& config, voice::AudioFrameSink cleaned_sink) {
    const Status config_status = voice::ValidateAudioAecConfig(config);
    if (!config_status.ok()) {
        return config_status;
    }
    const Status profile_status = profile_.Validate();
    if (!profile_status.ok()) {
        return profile_status;
    }
    if (!profile_.input_reference) {
        return Unavailable("当前 AudioBoard Profile 没有已验证的 playback reference");
    }
    if (!cleaned_sink) {
        return Status::Error(ErrorCode::kInvalidArgument, "AEC 必须绑定清理后音频回调");
    }
    // ESP-SR AFE create/feed/fetch 将在独占 ProcessingTask 中实现。
    return Unavailable("ESP-SR AEC ProcessingTask 尚未接入");
}

Status EspAecService::SubmitCapture(voice::AudioFrame) { return Unavailable("ESP-SR AEC ProcessingTask 尚未接入"); }

Status EspAecService::SubmitPlaybackReference(voice::AudioFrame) {
    return Unavailable("ESP-SR AEC ProcessingTask 尚未接入");
}

Status EspAecService::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.has_value()) {
        return Unavailable("AEC 尚未启动");
    }
    enabled_ = enabled;
    return Unavailable("ESP-SR AEC ProcessingTask 尚未接入");
}

Status EspAecService::Reset(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.has_value()) {
        config_->generation = generation;
    }
    stats_ = {};
    return Status::Ok();
}

Status EspAecService::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.reset();
    cleaned_sink_ = {};
    enabled_ = false;
    return Status::Ok();
}

voice::AudioAecStats EspAecService::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace voicelife::audio_esp
