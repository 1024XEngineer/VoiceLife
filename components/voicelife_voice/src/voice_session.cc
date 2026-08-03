#include "voicelife/voice/voice_session.h"

#include <string>
#include <utility>

namespace voicelife::voice {

VoiceSession::VoiceSession(AudioInputPort& input, AudioOutputPort& output, SpeechProviderAdapter& provider,
                           EvidenceSink evidence)
    : input_(input), output_(output), provider_(provider), evidence_(std::move(evidence)) {}

void VoiceSession::Emit(std::string_view event, std::string_view detail) {
    VoiceEvidence evidence;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        evidence = {.session_id = config_.session_id,
                    .generation = generation_,
                    .event = std::string(event),
                    .detail = std::string(detail)};
    }
    if (evidence_) {
        evidence_(evidence);
    }
}

VoiceSessionState VoiceSession::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

uint64_t VoiceSession::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

VoiceSessionConfig VoiceSession::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool VoiceSession::AcceptFrameLocked(const AudioFrame& frame) const {
    const AudioFormat& expected = config_.audio;
    const AudioFormat& actual = frame.format;
    return state_ == VoiceSessionState::kCapturing && frame.generation == generation_ && actual.valid() &&
           actual.codec == expected.codec && actual.sample_rate_hz == expected.sample_rate_hz &&
           actual.channels == expected.channels && actual.bits_per_sample == expected.bits_per_sample &&
           actual.frame_duration_ms == expected.frame_duration_ms && !frame.payload.empty();
}

Status VoiceSession::Start(const VoiceSessionConfig& config) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (config.session_id.empty() || config.provider_id.empty() || !config.audio.valid() ||
        config.hello_timeout_ms == 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "语音会话配置无效");
    }
    if (provider_.capabilities().provider_id != config.provider_id) {
        return Status::Error(ErrorCode::kInvalidArgument, "Provider ID 与能力 Profile 不一致");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kStopped && state_ != VoiceSessionState::kFailed) {
            return Status::Error(ErrorCode::kConflict, "语音会话已经启动");
        }
        config_ = config;
        state_ = VoiceSessionState::kStarting;
        generation_++;
        config_.generation = generation_;
        next_sequence_ = 0;
    }
    Status status = input_.Open(config_.audio);
    if (!status.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("input_open_failed", status.message);
        return status;
    }
    status = output_.Open(config_.audio);
    if (!status.ok()) {
        input_.Close();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("output_open_failed", status.message);
        return status;
    }
    provider_.SetAudioSink([this](AudioFrame frame) { return HandleAudio(std::move(frame)); });
    VoiceSessionConfig provider_config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider_config = config_;
    }
    status = provider_.Connect(provider_config,
                               [this](const VoiceEvent& event) { HandleEvent(event); });
    if (!status.ok()) {
        output_.Close();
        input_.Close();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("provider_connect_failed", status.message);
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = VoiceSessionState::kReady;
    }
    Emit("ready", provider_.capabilities().provider_id);
    return Status::Ok();
}

void VoiceSession::HandleEvent(const VoiceEvent& event) {
    bool stale = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A zero generation is not a wildcard: late events must not mutate a
        // session after interrupt or stop invalidated the old epoch.
        if (event.generation != generation_) {
            stale = true;
        } else if (event.kind == VoiceEventKind::kTtsStarted) {
            state_ = VoiceSessionState::kSpeaking;
        } else if (event.kind == VoiceEventKind::kTtsStopped &&
                   state_ == VoiceSessionState::kSpeaking) {
            state_ = VoiceSessionState::kReady;
        }
    }
    if (stale) {
        Emit("stale_event_dropped", "provider event generation mismatch");
    }
}

Status VoiceSession::BeginCapture() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    VoiceMode mode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kReady) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能开始采集");
        }
        mode = config_.mode;
    }
    Status status = provider_.StartCapture(mode);
    if (!status.ok()) {
        return status;
    }
    status = input_.StartCapture(mode);
    if (status.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kCapturing;
            next_sequence_ = 0;
        }
        Emit("capture_started", "");
    } else {
        provider_.StopCapture();
    }
    return status;
}

Status VoiceSession::EndCapture() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kCapturing) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前没有采集");
        }
    }
    Status input_status = input_.StopCapture();
    Status provider_status = provider_.StopCapture();
    if (input_status.ok() && provider_status.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kReady;
        }
        Emit("capture_stopped", "");
    }
    return !input_status.ok() ? input_status : provider_status;
}

Status VoiceSession::SubmitAudio(AudioFrame frame) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!AcceptFrameLocked(frame)) {
            return Status::Error(ErrorCode::kInvalidArgument, "音频帧属于旧会话、乱序或格式无效");
        }
        if (frame.sequence != next_sequence_) {
            return Status::Error(ErrorCode::kConflict, "音频帧序号不连续");
        }
    }
    Status status = provider_.SendAudio(frame);
    if (status.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++next_sequence_;
    }
    return status;
}

Status VoiceSession::HandleAudio(AudioFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != VoiceSessionState::kReady && state_ != VoiceSessionState::kSpeaking) {
        return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能播放音频");
    }
    if (frame.generation != generation_ || frame.format.codec != config_.audio.codec ||
        frame.format.sample_rate_hz != config_.audio.sample_rate_hz ||
        frame.format.channels != config_.audio.channels ||
        frame.format.bits_per_sample != config_.audio.bits_per_sample ||
        frame.format.frame_duration_ms != config_.audio.frame_duration_ms || frame.payload.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "播放帧不属于当前会话");
    }
    return output_.Push(frame);
}

Status VoiceSession::Speak(std::string_view text) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kReady || text.empty()) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能播报");
        }
        state_ = VoiceSessionState::kSpeaking;
    }
    Status status = provider_.Speak(text);
    if (!status.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = VoiceSessionState::kReady;
    }
    return status;
}

Status VoiceSession::Interrupt() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    bool capturing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kCapturing && state_ != VoiceSessionState::kSpeaking) {
            return Status::Ok();
        }
        capturing = state_ == VoiceSessionState::kCapturing;
    }
    Status input_status = Status::Ok();
    if (capturing) {
        input_status = input_.StopCapture();
    }
    Status status = provider_.Abort("user_interrupt");
    Status flush_status;
    uint64_t generation = 0;
    {
        // HandleAudio uses the same mutex. A frame either reaches the output
        // before this Flush and is removed, or observes the new generation
        // after the critical section and is rejected.
        std::lock_guard<std::mutex> lock(mutex_);
        flush_status = output_.Flush();
        ++generation_;
        config_.generation = generation_;
        next_sequence_ = 0;
        state_ = VoiceSessionState::kReady;
        generation = generation_;
    }
    provider_.SetGeneration(generation);
    Emit("interrupted", "old audio generation invalidated");
    if (!input_status.ok()) {
        return input_status;
    }
    return !status.ok() ? status : flush_status;
}

Status VoiceSession::Stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == VoiceSessionState::kStopped) {
            return Status::Ok();
        }
        ++generation_;
        config_.generation = generation_;
        next_sequence_ = 0;
        state_ = VoiceSessionState::kStopped;
        generation = generation_;
    }
    provider_.SetGeneration(generation);
    Status provider_status = provider_.Disconnect();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_.Close();
        input_.Close();
    }
    Emit("stopped", "");
    return provider_status;
}

}  // namespace voicelife::voice
