#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "voicelife/linx/linx_types.h"

namespace voicelife::linx {

/** @brief 处理 Linx MCP JSON-RPC payload 并返回完整响应消息。 */
using LinxMcpMessageHandler = std::function<Result<std::string>(std::string_view payload, std::string_view session_id)>;

/** 将 Linx 协议和传输适配为稳定的语音 Provider 契约。 */
class LinxSpeechProviderAdapter final : public voice::SpeechProviderAdapter {
   public:
    /**
     * @brief 创建 Linx Provider 适配器。
     * @param transport Linx 传输端口。
     * @param codec Linx 协议编解码端口。
     * @param connection Linx 连接配置。
     * @param capabilities Provider 能力声明。
     */
    LinxSpeechProviderAdapter(LinxTransportPort& transport, LinxProtocolCodecPort& codec,
                              LinxConnectionConfig connection,
                              voice::CapabilityProfile capabilities = DefaultCapabilities(),
                              LinxMcpMessageHandler mcp_handler = {},
                              std::unique_ptr<voice::CodecStrategy> codec_strategy = {});
    /** @brief 停止 MCP worker 并断开底层 Linx 传输。 */
    ~LinxSpeechProviderAdapter() override;

    /**
     * @brief 设置下行音频接收回调。
     * @param sink 接收解码音频帧的回调。
     */
    void SetAudioSink(voice::AudioFrameSink sink) override;
    /**
     * @brief 设置当前会话代次。
     * @param generation 当前会话代次。
     */
    void SetGeneration(uint64_t generation) override;
    /**
     * @brief 建立 Linx Provider 会话。
     * @param config 语音会话配置。
     * @param sink 接收语音事件的回调。
     * @return 连接结果。
     */
    Status Connect(const voice::VoiceSessionConfig& config, voice::VoiceEventSink sink) override;
    /** @brief 开始指定模式的音频采集。 @param mode 采集模式。 @return 启动结果。 */
    Status StartCapture(voice::VoiceMode mode) override;
    /** @brief 停止当前音频采集。 @return 停止结果。 */
    Status StopCapture() override;
    /**
     * @brief 发送一帧上行音频。
     * @param frame 待发送的音频帧。
     * @return 发送结果。
     */
    Status SendAudio(voice::AudioFrame frame) override;
    /** @brief 中止当前 Linx 会话。 @param reason 中止原因。 @return 中止结果。 */
    Status Abort(std::string_view reason) override;
    /** @brief 请求 Linx 合成文本。 @param text 待合成文本。 @return 请求结果。 */
    Status Speak(std::string_view text) override;
    /** @brief 发送本地唤醒确认，触发 Linx listen.detect。
     * @param wake_word 已由板端确认的唤醒词。
     * @param text_response 可选的服务端 TTS 确认文本。
     * @return 消息发送结果。
     */
    Status NotifyLocalWakeWord(std::string_view wake_word, std::string_view text_response = {}) override;
    /** @brief 断开 Linx Provider 会话。 @return 断开结果。 */
    Status Disconnect() override;
    /**
     * @brief 返回 Linx 协商出的音频格式。
     * @return 双向音频格式或错误。
     */
    [[nodiscard]] Result<voice::VoiceAudioFormats> audio_formats() const override;
    /** @brief 返回 Linx Provider 能力声明。 @return 能力声明的只读引用。 */
    [[nodiscard]] const voice::CapabilityProfile& capabilities() const override { return capabilities_; }

    /** @brief 返回 Linx Provider 的默认能力集合。 @return 默认能力声明。 */
    static voice::CapabilityProfile DefaultCapabilities();

   private:
    void OnText(std::string_view message);
    void OnBinary(std::vector<uint8_t> payload);
    void OnTransportConnected();
    void OnTransportDisconnected();
    void StartMcpWorker();
    void StopMcpWorker();
    void McpWorkerLoop();
    [[nodiscard]] voice::VoiceSessionConfig ActiveSessionConfig() const;
    [[nodiscard]] voice::AudioFormat WireAudioFormat() const;
    [[nodiscard]] Status ConfigureCodecStrategy();
    Status Send(Result<std::string> encoded);
    void Emit(voice::VoiceEvent event);
    LinxTransportPort& transport_;
    LinxProtocolCodecPort& codec_;
    LinxConnectionConfig connection_;
    voice::CapabilityProfile capabilities_;
    LinxMcpMessageHandler mcp_handler_;
    std::unique_ptr<voice::CodecStrategy> codec_strategy_;
    voice::VoiceSessionConfig config_;
    voice::VoiceEventSink event_sink_;
    voice::AudioFrameSink audio_sink_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> output_sequence_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> transport_connected_{false};
    std::atomic<bool> explicit_disconnect_{false};
    mutable std::mutex callback_mutex_;
    mutable std::mutex hello_mutex_;
    std::condition_variable hello_cv_;
    bool hello_received_ = false;
    bool audio_formats_ready_ = false;
    bool has_negotiated_formats_ = false;
    std::optional<std::string> remote_session_id_;
    voice::VoiceAudioFormats audio_formats_;
    voice::VoiceAudioFormats last_audio_formats_;
    Status hello_status_ = Status::Ok();
    /** 表示等待异步 MCP worker 处理的请求。 */
    struct McpRequest {
        std::string payload;
        std::string session_id;
        uint64_t generation = 0;
    };
    static constexpr std::size_t kMcpQueueCapacity = 4;
    std::mutex mcp_mutex_;
    std::condition_variable mcp_cv_;
    std::deque<McpRequest> mcp_queue_;
    bool mcp_stop_ = false;
    std::thread mcp_worker_;
};

}  // namespace voicelife::linx
