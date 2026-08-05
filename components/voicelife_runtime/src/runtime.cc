#include "voicelife/runtime/runtime.h"

#include "voicelife/mcp/mcp_server.h"
#include "voicelife/voice/voice_session_coordinator.h"

namespace voicelife::runtime {
namespace {

class ScaffoldAudioAdapter final : public voice::AudioDevicePort {
   public:
    Status Open() override { return Status::Ok(); }
    void Close() override {}
};

class ScaffoldSpeechAdapter final : public voice::SpeechProviderPort {
   public:
    Status Connect() override { return Status::Ok(); }
    void Disconnect() override {}
};

class McpVoiceBridge final : public voice::ToolGatewayPort {
   public:
    explicit McpVoiceBridge(mcp::McpServer& gateway) : gateway_(gateway) {}
    ToolResult Call(const ToolCall& call) override { return gateway_.call(call); }

   private:
    mcp::McpServer& gateway_;
};

class Runtime final {
   public:
    Runtime() : mcp_voice_bridge_(mcp_), voice_(audio_, speech_, mcp_voice_bridge_) {}

    Status Start() { return voice_.Start(); }

   private:
    mcp::McpServer mcp_;
    McpVoiceBridge mcp_voice_bridge_;
    ScaffoldAudioAdapter audio_;
    ScaffoldSpeechAdapter speech_;
    voice::VoiceSessionCoordinator voice_;
};

}  // namespace

Status Start() {
    static Runtime runtime;
    return runtime.Start();
}

}  // namespace voicelife::runtime
