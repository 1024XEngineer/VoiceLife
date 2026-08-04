#pragma once

#include "voicelife/contracts/tool.h"

namespace voicelife::voice {

/// 表示语音会话的生命周期状态。
enum class SessionState { kStopped, kStarting, kReady, kFailed };

/// 打开和关闭音频采集或播放设备的边界。
class AudioDevicePort {
   public:
    /** @brief 允许通过接口类型释放端口。 */
    virtual ~AudioDevicePort() = default;
    /** @brief 为当前会话打开音频设备。 @return 设备打开结果。 */
    virtual Status Open() = 0;
    /** @brief 关闭音频设备并释放资源。 */
    virtual void Close() = 0;
};

/// 连接语音识别或语音服务的提供方边界。
class SpeechProviderPort {
   public:
    /** @brief 允许通过接口类型释放端口。 */
    virtual ~SpeechProviderPort() = default;
    /** @brief 为会话连接语音服务。 @return 连接结果。 */
    virtual Status Connect() = 0;
    /** @brief 会话结束后断开语音服务。 */
    virtual void Disconnect() = 0;
};

/// 供语音编排使用的工具分发边界。
class ToolGatewayPort {
   public:
    /** @brief 允许通过接口类型释放端口。 */
    virtual ~ToolGatewayPort() = default;
    /**
     * @brief 路由工具调用并返回语义化结果。
     * @param call 来自语音编排的工具调用。
     * @return 工具调用的语义化结果。
     */
    virtual ToolResult Call(const ToolCall& call) = 0;
};

/// 编排语音会话中的音频、语音服务和工具分发。
class VoiceSessionCoordinator {
   public:
    /**
     * @brief 使用服务依赖创建会话协调器。
     * @param audio 会话使用的音频设备。
     * @param speech 会话使用的语音服务。
     * @param tools 分发语音指令的工具网关。
     */
    VoiceSessionCoordinator(AudioDevicePort& audio, SpeechProviderPort& speech, ToolGatewayPort& tools)
        : audio_(audio), speech_(speech), tools_(tools) {}

    /** @brief 启动服务，成功后切换到 ready 状态。 @return 启动结果。 */
    Status Start();
    /** @brief 停止服务并将会话恢复为 stopped 状态。 */
    void Stop();
    /**
     * @brief 会话活动期间分发工具调用。
     * @param call 语音流程产生的工具调用。
     * @return 工具调用的语义化结果。
     */
    ToolResult DispatchToolCall(const ToolCall& call);
    /** @brief 返回当前语音会话状态。 @return 当前生命周期状态。 */
    [[nodiscard]] SessionState state() const { return state_; }

   private:
    AudioDevicePort& audio_;
    SpeechProviderPort& speech_;
    ToolGatewayPort& tools_;
    SessionState state_ = SessionState::kStopped;
};

}  // namespace voicelife::voice
