#include "voicelife/runtime/runtime.h"

#include "voicelife/mcp/mcp_tool_gateway.h"
#include "voicelife/voice/voice_session_coordinator.h"

namespace voicelife::runtime {
namespace {

// 提供可启动的音频设备占位适配器。
class ScaffoldAudioAdapter final : public voice::AudioDevicePort {
   public:
    Status Open() override { return Status::Ok(); }
    void Close() override {}
};

// 提供可连接的语音服务占位适配器。
class ScaffoldSpeechAdapter final : public voice::SpeechProviderPort {
   public:
    Status Connect() override { return Status::Ok(); }
    void Disconnect() override {}
};

// 将语音工具调用转发给通用 MCP 注册中心。
class McpVoiceBridge final : public voice::ToolGatewayPort {
   public:
    explicit McpVoiceBridge(mcp::McpToolGateway& gateway) : gateway_(gateway) {}
    ToolResult Call(const ToolCall& call) override { return gateway_.call(call); }

   private:
    mcp::McpToolGateway& gateway_;
};

// 组装当前可用的语音和 MCP 基础能力。
class Runtime final {
   public:
    Runtime() : mcp_voice_bridge_(mcp_), voice_(audio_, speech_, mcp_voice_bridge_) {}
    Status Start() { return voice_.Start(); }

   private:
    mcp::McpToolGateway mcp_;
    McpVoiceBridge mcp_voice_bridge_;
    ScaffoldAudioAdapter audio_;
    ScaffoldSpeechAdapter speech_;
    voice::VoiceSessionCoordinator voice_;
};

}  // namespace

// 启动全局运行时实例。
Status Start() {
    static Runtime runtime;
    return runtime.Start();
}

}  // namespace voicelife::runtime
