#include "support/test_support.h"
#include "voicelife/mcp/mcp_tool_gateway.h"
#include "voicelife/voice/voice_session_coordinator.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::ToolCall;
using voicelife::ToolResult;
using voicelife::test::Check;

namespace {

class ReadyAudio final : public voicelife::voice::AudioDevicePort {
   public:
    Status Open() override { return Status::Ok(); }
    void Close() override {}
};

class ReadySpeech final : public voicelife::voice::SpeechProviderPort {
   public:
    Status Connect() override { return Status::Ok(); }
    void Disconnect() override {}
};

class McpBridge final : public voicelife::voice::ToolGatewayPort {
   public:
    explicit McpBridge(voicelife::mcp::McpToolGateway& gateway) : gateway_(gateway) {}
    ToolResult Call(const ToolCall& call) override { return gateway_.call(call); }

   private:
    voicelife::mcp::McpToolGateway& gateway_;
};

}  // namespace

int main() {
    voicelife::mcp::McpToolGateway mcp;
    ReadyAudio audio;
    ReadySpeech speech;
    McpBridge tools(mcp);
    voicelife::voice::VoiceSessionCoordinator voice(audio, speech, tools);

    Check(voice.Start().ok(), "Runtime 主链应可启动");
    Check(mcp.list_tools().total == 0, "Runtime 当前不应预注册业务工具");
    const auto result =
        voice.DispatchToolCall({.request_id = "request-1", .name = "voicelife.unknown", .arguments = {}});
    Check(result.status.code == ErrorCode::kNotFound, "空注册中心应拒绝未知工具调用");
    return 0;
}
