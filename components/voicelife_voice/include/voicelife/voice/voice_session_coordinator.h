#pragma once

#include "voicelife/contracts/tool.h"

namespace voicelife::voice {

/// Represents the lifecycle state of a voice session.
enum class SessionState { kStopped, kStarting, kReady, kFailed };

/// Device boundary for opening and closing audio capture or playback.
class AudioDevicePort {
   public:
    /** @brief Releases the port through its interface type. */
    virtual ~AudioDevicePort() = default;
    /** @brief Opens the audio device for the current session. @return Device-open result. */
    virtual Status Open() = 0;
    /** @brief Closes the audio device and releases its resources. */
    virtual void Close() = 0;
};

/// Provider boundary for connecting voice-recognition or speech services.
class SpeechProviderPort {
   public:
    /** @brief Releases the port through its interface type. */
    virtual ~SpeechProviderPort() = default;
    /** @brief Connects the speech provider for a session. @return Connection result. */
    virtual Status Connect() = 0;
    /** @brief Disconnects the speech provider after a session. */
    virtual void Disconnect() = 0;
};

/// Tool-dispatch boundary used by voice orchestration.
class ToolGatewayPort {
   public:
    /** @brief Releases the port through its interface type. */
    virtual ~ToolGatewayPort() = default;
    /**
     * @brief Routes a tool call and returns its semantic result.
     * @param call Tool invocation from voice orchestration.
     * @return Semantic result of the invocation.
     */
    virtual ToolResult Call(const ToolCall& call) = 0;
};

/// Coordinates audio, speech, and tool dispatch for a voice session.
class VoiceSessionCoordinator {
   public:
    /**
     * @brief Creates a coordinator from its service dependencies.
     * @param audio Audio device used by the session.
     * @param speech Speech provider used by the session.
     * @param tools Tool gateway used to dispatch voice commands.
     */
    VoiceSessionCoordinator(AudioDevicePort& audio, SpeechProviderPort& speech, ToolGatewayPort& tools)
        : audio_(audio), speech_(speech), tools_(tools) {}

    /** @brief Starts services and transitions to ready on success. @return Startup result. */
    Status Start();
    /** @brief Stops services and returns the session to stopped state. */
    void Stop();
    /**
     * @brief Dispatches a tool call while the session is active.
     * @param call Tool invocation from the voice flow.
     * @return Semantic result of the invocation.
     */
    ToolResult DispatchToolCall(const ToolCall& call);
    /** @brief Returns the current voice-session state. @return Current lifecycle state. */
    [[nodiscard]] SessionState state() const { return state_; }

   private:
    AudioDevicePort& audio_;
    SpeechProviderPort& speech_;
    ToolGatewayPort& tools_;
    SessionState state_ = SessionState::kStopped;
};

}  // namespace voicelife::voice
