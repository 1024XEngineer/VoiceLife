#include "voicelife/voice/voice_session_coordinator.h"

namespace voicelife::voice {

Status VoiceSessionCoordinator::Start() {
    if (state_ == SessionState::kReady) {
        return Status::Ok();
    }
    state_ = SessionState::kStarting;
    Status status = audio_.Open();
    if (!status.ok()) {
        state_ = SessionState::kFailed;
        return status;
    }
    status = speech_.Connect();
    if (!status.ok()) {
        audio_.Close();
        state_ = SessionState::kFailed;
        return status;
    }
    state_ = SessionState::kReady;
    return Status::Ok();
}

void VoiceSessionCoordinator::Stop() {
    speech_.Disconnect();
    audio_.Close();
    state_ = SessionState::kStopped;
}

ToolResult VoiceSessionCoordinator::DispatchToolCall(const ToolCall& call) {
    if (state_ != SessionState::kReady) {
        return ToolResult::Failure(Status::Error(ErrorCode::kUnavailable, "语音会话尚未就绪"));
    }
    return tools_.Call(call);
}

}  // namespace voicelife::voice
