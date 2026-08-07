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
    const AudioFormat& expected = audio_formats_.capture;
    const AudioFormat& actual = frame.format;
    return state_ == VoiceSessionState::kCapturing && frame.generation == generation_ && actual.valid() &&
           actual.codec == expected.codec && actual.sample_rate_hz == expected.sample_rate_hz &&
           actual.channels == expected.channels && actual.bits_per_sample == expected.bits_per_sample &&
           actual.frame_duration_ms == expected.frame_duration_ms && !frame.payload.empty() &&
           frame.payload.size() <= AudioFrame::kMaxPayloadBytes;
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
        audio_formats_ = {.capture = config.audio, .playback = config.audio};
        audio_ready_ = false;
        state_ = VoiceSessionState::kStarting;
        generation_++;
        config_.generation = generation_;
        next_sequence_ = 0;
    }
    provider_.SetAudioSink([this](AudioFrame frame) { return HandleAudio(std::move(frame)); });
    VoiceSessionConfig provider_config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider_config = config_;
    }
    Status status = provider_.Connect(provider_config, [this](const VoiceEvent& event) { HandleEvent(event); });
    if (!status.ok()) {
        provider_.SetAudioSink({});
        (void)provider_.Disconnect();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("provider_connect_failed", status.message);
        return status;
    }
    auto negotiated = provider_.audio_formats();
    if (!negotiated.ok() || !negotiated.value.has_value() || !negotiated.value->valid()) {
        provider_.SetAudioSink({});
        provider_.Disconnect();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        const Status failure = negotiated.ok()
                                   ? Status::Error(ErrorCode::kInvalidArgument, "Provider 返回的双向音频格式无效")
                                   : negotiated.status;
        Emit("audio_negotiation_failed", failure.message);
        return failure;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_formats_ = *negotiated.value;
    }
    input_.SetAudioSink([this](AudioFrame frame) { return HandleInputAudio(std::move(frame)); });
    status = input_.Open(negotiated.value->capture);
    if (!status.ok()) {
        input_.SetAudioSink({});
        provider_.SetAudioSink({});
        provider_.Disconnect();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("input_open_failed", status.message);
        return status;
    }
    status = output_.Open(negotiated.value->playback);
    if (!status.ok()) {
        input_.SetAudioSink({});
        provider_.SetAudioSink({});
        input_.Close();
        provider_.Disconnect();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("output_open_failed", status.message);
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_ready_ = true;
        state_ = VoiceSessionState::kReady;
    }
    Emit("ready", provider_.capabilities().provider_id);
    return Status::Ok();
}

void VoiceSession::HandleEvent(const VoiceEvent& event) {
    bool stale = false;
    bool generation_changed = false;
    bool disconnected = false;
    bool playback_aborted = false;
    Status flush_status = Status::Ok();
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A zero generation is not a wildcard: late events must not mutate a
        // session after interrupt or stop invalidated the old epoch.
        if (event.generation != generation_) {
            stale = true;
        } else if (event.kind == VoiceEventKind::kDisconnected && audio_ready_ &&
                   state_ != VoiceSessionState::kStopped && state_ != VoiceSessionState::kFailed) {
            ++generation_;
            config_.generation = generation_;
            next_sequence_ = 0;
            state_ = VoiceSessionState::kStarting;
            generation = generation_;
            generation_changed = true;
            disconnected = true;
        } else if (event.kind == VoiceEventKind::kConnected && audio_ready_ && state_ == VoiceSessionState::kStarting) {
            state_ = VoiceSessionState::kReady;
        } else if (event.kind == VoiceEventKind::kTtsStarted) {
            state_ = VoiceSessionState::kSpeaking;
        } else if (event.kind == VoiceEventKind::kTtsStopped) {
            if (event.aborted && audio_ready_) {
                ++generation_;
                config_.generation = generation_;
                next_sequence_ = 0;
                state_ = VoiceSessionState::kReady;
                generation = generation_;
                playback_aborted = true;
            } else if (state_ == VoiceSessionState::kSpeaking) {
                state_ = VoiceSessionState::kReady;
            }
        }
    }
    if (stale) {
        Emit("stale_event_dropped", "provider event generation mismatch");
    } else if (generation_changed) {
        provider_.SetGeneration(generation);
        Emit("transport_disconnected", "audio sending disabled until a new hello completes");
    } else if (playback_aborted) {
        provider_.SetGeneration(generation);
        flush_status = output_.Flush();
        Emit("tts_aborted", "server abort invalidated buffered playback");
        if (!flush_status.ok()) {
            Emit("playback_flush_failed", flush_status.message);
        }
    } else if (event.kind == VoiceEventKind::kConnected) {
        Emit("transport_connected", "provider hello completed");
    }
    if (disconnected) {
        return;
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
    Status provider_status = provider_.StartCapture(mode);
    if (!provider_status.ok()) {
        return provider_status;
    }
    Status input_status = input_.StartCapture(mode);
    if (input_status.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kCapturing;
            next_sequence_ = 0;
        }
        Emit("capture_started", "");
        return Status::Ok();
    }
    // Input failed after the provider already started listening. The provider
    // must be stopped so the server does not stay in a half-open capture state.
    Status rollback = provider_.StopCapture();
    if (!rollback.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("capture_rollback_failed", rollback.message);
        return rollback;
    }
    return input_status;
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
        return Status::Ok();
    }
    // Input is already stopped but provider stop failed: the session cannot
    // safely return to kReady or kCapturing. Transition to kFailed so the
    // caller does not attempt further capture on a half-closed session.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = VoiceSessionState::kFailed;
    }
    const Status failure = !input_status.ok() ? input_status : provider_status;
    Emit("capture_stop_failed", failure.message);
    return failure;
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

Status VoiceSession::HandleInputAudio(AudioFrame frame) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    uint64_t generation = 0;
    uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kCapturing) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能发送采集音频");
        }
        const AudioFormat& expected = audio_formats_.capture;
        if (!frame.format.valid() || frame.format.codec != expected.codec ||
            frame.format.sample_rate_hz != expected.sample_rate_hz || frame.format.channels != expected.channels ||
            frame.format.bits_per_sample != expected.bits_per_sample ||
            frame.format.frame_duration_ms != expected.frame_duration_ms || frame.payload.empty() ||
            frame.payload.size() > AudioFrame::kMaxPayloadBytes) {
            return Status::Error(ErrorCode::kInvalidArgument, "采集帧格式与会话不一致");
        }
        generation = generation_;
        sequence = next_sequence_;
        frame.generation = generation;
        frame.sequence = sequence;
    }
    Status status = provider_.SendAudio(frame);
    if (status.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == VoiceSessionState::kCapturing && generation_ == generation && next_sequence_ == sequence) {
            ++next_sequence_;
        }
    }
    return status;
}

Status VoiceSession::HandleAudio(AudioFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != VoiceSessionState::kReady && state_ != VoiceSessionState::kSpeaking) {
        return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能播放音频");
    }
    const AudioFormat& expected = audio_formats_.playback;
    if (frame.generation != generation_ || frame.format.codec != expected.codec ||
        frame.format.sample_rate_hz != expected.sample_rate_hz || frame.format.channels != expected.channels ||
        frame.format.bits_per_sample != expected.bits_per_sample ||
        frame.format.frame_duration_ms != expected.frame_duration_ms || frame.payload.empty() ||
        frame.payload.size() > AudioFrame::kMaxPayloadBytes) {
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
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kCapturing && state_ != VoiceSessionState::kSpeaking) {
            return Status::Ok();
        }
        capturing = state_ == VoiceSessionState::kCapturing;
        // Invalidate the old generation first. Late frames that arrive during
        // the subsequent Abort/Flush window are rejected by HandleAudio (which
        // checks generation_) and HandleInputAudio (which checks state_).
        ++generation_;
        config_.generation = generation_;
        next_sequence_ = 0;
        state_ = VoiceSessionState::kReady;
        generation = generation_;
    }
    // Notify the provider before tearing down so it can drop stale frames.
    provider_.SetGeneration(generation);
    Status input_status = Status::Ok();
    if (capturing) {
        input_status = input_.StopCapture();
    }
    Status abort_status = provider_.Abort("user_interrupt");
    Status flush_status = output_.Flush();
    Emit("interrupted", "old audio generation invalidated");
    if (!input_status.ok()) return input_status;
    if (!abort_status.ok()) return abort_status;
    return flush_status;
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
        audio_ready_ = false;
        state_ = VoiceSessionState::kStopped;
        generation = generation_;
    }
    provider_.SetGeneration(generation);
    provider_.SetAudioSink({});
    Status provider_status = provider_.Disconnect();
    output_.Close();
    input_.SetAudioSink({});
    input_.Close();
    if (!provider_status.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("stop_disconnect_failed", provider_status.message);
    } else {
        Emit("stopped", "");
    }
    return provider_status;
}

}  // namespace voicelife::voice
