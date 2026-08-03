#include "support/calendar_fakes.h"
#include "support/test_support.h"
#include "voicelife/im/im_gateway_adapter.h"
#include "voicelife/mcp/mcp_tool_gateway.h"
#include "voicelife/platform/in_memory_calendar_store.h"
#include "voicelife/voice/voice_session_coordinator.h"

using voicelife::Status;
using voicelife::ToolCall;
using voicelife::ToolResult;
using voicelife::test::Check;

namespace {

class DisabledTransport final : public voicelife::im::ImTransportPort {
   public:
    Status Send(const voicelife::im::ImGatewayRequest&) override {
        return Status::Error(voicelife::ErrorCode::kUnavailable, "测试通道未配置");
    }
};

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
    ToolResult Call(const ToolCall& call) override { return gateway_.Call(call); }

   private:
    voicelife::mcp::McpToolGateway& gateway_;
};

}  // namespace

int main() {
    voicelife::platform::InMemoryCalendarStore store;
    voicelife::test::SequenceIds ids;
    voicelife::test::FixedClock clock(1785740000);
    DisabledTransport transport;
    voicelife::im::ImGatewayAdapter notifications(transport);
    voicelife::application::CalendarApplication calendar(store, notifications, ids, clock);
    voicelife::mcp::McpToolGateway mcp(calendar);
    ReadyAudio audio;
    ReadySpeech speech;
    McpBridge tools(mcp);
    voicelife::voice::VoiceSessionCoordinator voice(audio, speech, tools);

    Check(voice.Start().ok(), "Runtime 主链应通过 Port 完成装配");
    const auto created = voice.DispatchToolCall({
        .request_id = "request-1",
        .name = "voicelife.schedule.create",
        .arguments = {{"title", "架构评审"}, {"starts_at", "1785747600"}},
    });
    Check(created.status.ok(), "Voice -> MCP -> Application 主链应可执行");
    Check(created.output.at("notification_accepted") == "false", "通知不可用时本地业务应降级成功");
    Check(store.Size() == 1, "主链应原子保存日程与定时任务");
    return 0;
}
