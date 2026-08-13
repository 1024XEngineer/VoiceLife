#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

#include "voicelife/voice/voice_ports.h"

namespace voicelife::voice {

/** 编排音频端口和 Provider 的单次语音会话。 */
class VoiceSession {
   public:
    /**
     * @brief 使用输入、输出和 Provider 端口创建语音会话。
     * @param input 音频输入端口。
     * @param output 音频输出端口。
     * @param provider 语音 Provider 适配器。
     * @param evidence 可选的审计证据回调。
     */
    VoiceSession(AudioInputPort& input, AudioOutputPort& output, SpeechProviderAdapter& provider,
                 EvidenceSink evidence = {});

    /**
     * @brief 连接 Provider 并准备音频设备。
     * @param config 会话配置。
     * @return 启动结果。
     */
    Status Start(const VoiceSessionConfig& config);
    /** @brief 开始采集音频。 @return 开始结果。 */
    Status BeginCapture();
    /** @brief 结束采集音频。 @return 结束结果。 */
    Status EndCapture();
    /**
     * @brief 提交一帧来自输入端口的音频。
     * @param frame 待提交的音频帧。
     * @return 提交结果。
     */
    Status SubmitAudio(AudioFrame frame);
    /**
     * @brief 处理一帧来自 Provider 的下行音频。
     * @param frame 待播放的音频帧。
     * @return 处理结果。
     */
    Status HandleAudio(AudioFrame frame);
    /**
     * @brief 上报已受控 MCP 工具开始执行的会话语义。
     *
     * Runtime 的 MCP worker 调用此入口；它不会访问 Provider、音频或显示，
     * 只经 EvidenceSink 投递给交互事件循环。
     */
    void ReportToolCallStarted();
    /**
     * @brief 上报已受控 MCP 工具结果的会话语义。
     * @param summary 已截断的用户可见结果摘要。
     * @param success 工具是否成功。
     */
    void ReportToolResult(std::string_view summary, bool success);
    /**
     * @brief 请求 Provider 合成文本。
     * @param text 待合成文本。
     * @return 请求结果。
     */
    Status Speak(std::string_view text);
    /** @brief 中断当前会话并推进会话代次。 @return 中断结果。 */
    Status Interrupt();
    /** @brief 停止会话并关闭所有音频资源。 @return 停止结果。 */
    Status Stop();

    /** @brief 返回当前会话状态。 @return 会话状态。 */
    [[nodiscard]] VoiceSessionState state() const;
    /** @brief 返回当前会话代次。 @return 会话代次。 */
    [[nodiscard]] uint64_t generation() const;
    /** @brief 返回当前会话配置快照。 @return 会话配置。 */
    [[nodiscard]] VoiceSessionConfig config() const;
    /** @brief 返回 Provider hello 协商后的下行播放格式。 @return 播放格式。 */
    [[nodiscard]] AudioFormat playback_format() const;

   private:
    void Emit(std::string_view event, std::string_view detail);
    void HandleEvent(const VoiceEvent& event);
    Status HandleInputAudio(AudioFrame frame);
    bool AcceptFrameLocked(const AudioFrame& frame) const;

    AudioInputPort& input_;
    AudioOutputPort& output_;
    SpeechProviderAdapter& provider_;
    EvidenceSink evidence_;
    // Serializes resource lifecycle operations. Provider callbacks only take
    // mutex_, so an event arriving from the transport worker cannot deadlock
    // Start/Interrupt/Stop.
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    VoiceSessionConfig config_;
    VoiceAudioFormats audio_formats_;
    VoiceSessionState state_ = VoiceSessionState::kStopped;
    bool audio_ready_ = false;
    // 本轮是否已收到有效输入（STT/工具调用），仅在其为 true 时接受服务端 TTS，
    // 避免空闲态误收上一轮残留回复。
    bool response_armed_ = false;
    // VAD 端点：本地静音检测（无 AFE，用 RMS 能量近似）。
    bool vad_speech_seen_ = false;
    bool vad_silence_emitted_ = false;
    bool vad_silence_pending_ = false;
    std::chrono::steady_clock::time_point last_speech_at_{};
    uint64_t generation_ = 0;
    uint64_t next_sequence_ = 0;
};

}  // namespace voicelife::voice
