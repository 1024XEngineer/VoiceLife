#pragma once

#include "voicelife/application/calendar_application.h"
#include "voicelife/im/im_gateway_adapter.h"
#include "voicelife/mcp/mcp_tool_gateway.h"
#include "voicelife/platform/in_memory_calendar_store.h"
#include "voicelife/platform/sequential_id_generator.h"
#include "voicelife/voice/voice_session_coordinator.h"

namespace voicelife::runtime {

class Runtime final {
   public:
    Runtime();
    Status Start();

   private:
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

    class DisabledImTransport final : public im::ImTransportPort {
       public:
        Status Send(const im::ImGatewayRequest&) override {
            return Status::Error(ErrorCode::kUnavailable, "IM 网络适配器尚未接入");
        }
    };

    class McpVoiceBridge final : public voice::ToolGatewayPort {
       public:
        explicit McpVoiceBridge(mcp::McpToolGateway& gateway) : gateway_(gateway) {}
        ToolResult Call(const ToolCall& call) override { return gateway_.Call(call); }

       private:
        mcp::McpToolGateway& gateway_;
    };

    platform::InMemoryCalendarStore store_;
    platform::SequentialIdGenerator ids_;
    DisabledImTransport im_transport_;
    im::ImGatewayAdapter im_gateway_;
    application::CalendarApplication calendar_;
    mcp::McpToolGateway mcp_;
    McpVoiceBridge mcp_voice_bridge_;
    ScaffoldAudioAdapter audio_;
    ScaffoldSpeechAdapter speech_;
    voice::VoiceSessionCoordinator voice_;
};

}  // namespace voicelife::runtime
