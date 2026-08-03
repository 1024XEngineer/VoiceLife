#include "voicelife/voice/voice_session.h"

#include <string>
#include <utility>

namespace voicelife::voice {

VoiceSession::VoiceSession(AudioInputPort& input, AudioOutputPort& output, SpeechProviderAdapter& provider,
                           EvidenceSink evidence)
    : input_(input), output_(output), provider_(provider), evidence_(std::move(evidence)) {}

void VoiceSession::Emit(std::string_view event, std::string_view detail) {
    if (evidence_) {
        evidence_({.session_id = config_.session_id,
                   .generation = generation_,
                   .event = std::string(event),
                   .detail = std::string(detail)});
    }
}

bool VoiceSession::AcceptFrame(const AudioFrame& frame) const {
    const AudioFormat& expected = config_.audio;
    const AudioFormat& actual = frame.format;
    return state_ == VoiceSessionState::kCapturing && frame.generation == generation_ &&
           actual.valid() && actual.codec == expected.codec &&
           actual.sample_rate_hz == expected.sample_rate_hz &&
           actual.channels == expected.channels &&
           actual.bits_per_sample == expected.bits_per_sample &&
           actual.frame_duration_ms == expected.frame_duration_ms && !frame.payload.empty();
}

Status VoiceSession::Start(const VoiceSessionConfig& config) {
    if (state_ != VoiceSessionState::kStopped && state_ != VoiceSessionState::kFailed) {
        return Status::Error(ErrorCode::kConflict, "语音会话已经启动");
    }
    if (config.session_id.empty() || config.provider_id.empty() || !config.audio.valid() ||
        config.hello_timeout_ms == 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "语音会话配置无效");
    }
    if (provider_.capabilities().provider_id != config.provider_id) {
        return Status::Error(ErrorCode::kInvalidArgument, "Provider ID 与能力 Profile 不一致");
    }

    config_ = config;
    state_ = VoiceSessionState::kStarting;
    generation_++;
    next_sequence_ = 0;
    Status status = input_.Open(config_.audio);
    if (!status.ok()) {
        state_ = VoiceSessionState::kFailed;
        Emit("input_open_failed", status.message);
        return status;
    }
    status = output_.Open(config_.audio);
    if (!status.ok()) {
        input_.Close();
        state_ = VoiceSessionState::kFailed;
        Emit("output_open_failed", status.message);
        return status;
    }
    status = provider_.Connect(config_, [this](const VoiceEvent& event) {
        // Every provider event belongs to one connection/session epoch. A zero
        // generation is not a wildcard: accepting it would let a late event
        // mutate state after interrupt or stop invalidated the old epoch.
        if (event.generation != generation_) {
            Emit("stale_event_dropped", "provider event generation mismatch");
            return;
        }
        if (event.kind == VoiceEventKind::kTtsStarted) {
            state_ = VoiceSessionState::kSpeaking;
        } else if (event.kind == VoiceEventKind::kTtsStopped && state_ == VoiceSessionState::kSpeaking) {
            state_ = VoiceSessionState::kReady;
        }
    });
    if (!status.ok()) {
        output_.Close();
        input_.Close();
        state_ = VoiceSessionState::kFailed;
        Emit("provider_connect_failed", status.message);
        return status;
    }
    state_ = VoiceSessionState::kReady;
    Emit("ready", provider_.capabilities().provider_id);
    return Status::Ok();
}

Status VoiceSession::BeginCapture() {
    if (state_ != VoiceSessionState::kReady) {
        return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能开始采集");
    }
    Status status = provider_.StartCapture(config_.mode);
    if (!status.ok()) {
        return status;
    }
    status = input_.StartCapture(config_.mode);
    if (status.ok()) {
        state_ = VoiceSessionState::kCapturing;
        next_sequence_ = 0;
        Emit("capture_started", "");
    } else {
        provider_.StopCapture();
    }
    return status;
}

Status VoiceSession::EndCapture() {
    if (state_ != VoiceSessionState::kCapturing) {
        return Status::Error(ErrorCode::kUnavailable, "语音会话当前没有采集");
    }
    Status input_status = input_.StopCapture();
    Status provider_status = provider_.StopCapture();
    if (input_status.ok() && provider_status.ok()) {
        state_ = VoiceSessionState::kReady;
        Emit("capture_stopped", "");
    }
    return !input_status.ok() ? input_status : provider_status;
}

Status VoiceSession::SubmitAudio(AudioFrame frame) {
    if (!AcceptFrame(frame)) {
        return Status::Error(ErrorCode::kInvalidArgument, "音频帧属于旧会话、乱序或格式无效");
    }
    if (frame.sequence != next_sequence_) {
        return Status::Error(ErrorCode::kConflict, "音频帧序号不连续");
    }
    Status status = provider_.SendAudio(frame);
    if (status.ok()) {
        ++next_sequence_;
    }
    return status;
}

Status VoiceSession::HandleAudio(AudioFrame frame) {
    if (state_ != VoiceSessionState::kReady && state_ != VoiceSessionState::kSpeaking) {
        return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能播放音频");
    }
    if (frame.generation != generation_ || frame.format.codec != config_.audio.codec || frame.payload.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "播放帧不属于当前会话");
    }
    return output_.Push(frame);
}

Status VoiceSession::Speak(std::string_view text) {
    if (state_ != VoiceSessionState::kReady || text.empty()) {
        return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能播报");
    }
    state_ = VoiceSessionState::kSpeaking;
    Status status = provider_.Speak(text);
    if (!status.ok()) {
        state_ = VoiceSessionState::kReady;
    }
    return status;
}

Status VoiceSession::Interrupt() {
    if (state_ != VoiceSessionState::kCapturing && state_ != VoiceSessionState::kSpeaking) {
        return Status::Ok();
    }
    Status input_status = Status::Ok();
    if (state_ == VoiceSessionState::kCapturing) {
        input_status = input_.StopCapture();
    }
    Status status = provider_.Abort("user_interrupt");
    Status flush_status = output_.Flush();
    ++generation_;
    next_sequence_ = 0;
    state_ = VoiceSessionState::kReady;
    Emit("interrupted", "old audio generation invalidated");
    if (!input_status.ok()) {
        return input_status;
    }
    return !status.ok() ? status : flush_status;
}

Status VoiceSession::Stop() {
    if (state_ == VoiceSessionState::kStopped) {
        return Status::Ok();
    }
    Status provider_status = provider_.Disconnect();
    output_.Close();
    input_.Close();
    ++generation_;
    state_ = VoiceSessionState::kStopped;
    Emit("stopped", "");
    return provider_status;
}

}  // namespace voicelife::voice
