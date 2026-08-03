#include "voicelife/runtime/runtime.h"

#include "voicelife/voice/voice_session_coordinator.h"

namespace voicelife::runtime {
namespace {

// 提供可启动的音频设备占位适配器。
class ScaffoldAudioAdapter final : public voice::AudioDevicePort {
   public:
    // 打开占位音频设备。
    Status Open() override { return Status::Ok(); }

    // 关闭占位音频设备。
    void Close() override {}
};

// 提供可连接的语音服务占位适配器。
class ScaffoldSpeechAdapter final : public voice::SpeechProviderPort {
   public:
    // 连接占位语音服务。
    Status Connect() override { return Status::Ok(); }

    // 断开占位语音服务。
    void Disconnect() override {}
};

// 日程工具尚未接入时拒绝所有工具调用。
class DisabledToolGateway final : public voice::ToolGatewayPort {
   public:
    // 在工具网关未接入期间返回不可用状态。
    ToolResult Call(const ToolCall&) override {
        return {.status = Status::Error(ErrorCode::kUnavailable, "工具网关尚未接入"), .output = {}};
    }
};

// 组装当前可用的语音运行时基础能力。
class Runtime final {
   public:
    Runtime() : voice_(audio_, speech_, tools_) {}

    // 启动语音会话。
    Status Start() { return voice_.Start(); }

   private:
    ScaffoldAudioAdapter audio_;
    ScaffoldSpeechAdapter speech_;
    DisabledToolGateway tools_;
    voice::VoiceSessionCoordinator voice_;
};

}  // namespace

// 启动全局运行时实例。
Status Start() {
    static Runtime runtime;
    return runtime.Start();
}

}  // namespace voicelife::runtime
