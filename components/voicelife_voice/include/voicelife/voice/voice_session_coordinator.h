#pragma once

#include "voicelife/contracts/tool.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::voice {

enum class SessionState { kStopped, kStarting, kReady, kFailed };

class AudioDevicePort {
   public:
    virtual ~AudioDevicePort() = default;
    virtual Status Open() = 0;
    virtual void Close() = 0;
};

class ToolGatewayPort {
   public:
    virtual ~ToolGatewayPort() = default;
    virtual ToolResult Call(const ToolCall& call) = 0;
};

class VoiceSessionCoordinator {
   public:
    VoiceSessionCoordinator(AudioDevicePort& audio, SpeechProviderPort& speech, ToolGatewayPort& tools)
        : audio_(audio), speech_(speech), tools_(tools) {}

    Status Start();
    void Stop();
    ToolResult DispatchToolCall(const ToolCall& call);
    [[nodiscard]] SessionState state() const { return state_; }

   private:
    AudioDevicePort& audio_;
    SpeechProviderPort& speech_;
    ToolGatewayPort& tools_;
    SessionState state_ = SessionState::kStopped;
};

}  // namespace voicelife::voice
