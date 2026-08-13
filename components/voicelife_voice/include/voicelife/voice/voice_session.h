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
     * MCP 应用的专属执行器调用此入口；它不会访问 Provider、音频或显示，
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
    /**
     * @brief 通知 Provider 本地已检测到唤醒词，并可请求一段确认播报。
     * @param wake_word 已由本地检测器确认的唤醒词。
     * @param text_response 可选的服务端 TTS 确认文本。
     * @return 请求结果。
     */
    Status NotifyLocalWakeWord(std::string_view wake_word, std::string_view text_response = {});
    /**
     * @brief 取消当前回合后，以新代次发送本地确认播报。
     *
     * 若正在播报，旧流的 tts.stop 是确认消息的顺序栅栏；旧 PCM 清空且终止
     * 标记到达后才发送确认，避免旧文本在“收到！”之后继续播放。
     * @param wake_word 已由本地检测器确认的唤醒词。
     * @param text_response 可选的服务端 TTS 确认文本。
     * @return 请求结果。
     */
    Status InterruptAndNotifyLocalWakeWord(std::string_view wake_word, std::string_view text_response);
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
    // 每段远端 TTS 仅上报一次首个成功入播放队列的音频帧，供 Runtime
    // 记录唤醒确认的端到端时延；不携带 PCM 或文本。
    bool first_tts_audio_pending_ = false;
    // EndCapture 仅停止本地 PCM；服务端最终 STT 会在随后到达。该标记允许
    // 同一 generation 的最终识别结果在 kReady 中被接收一次，避免 VAD 端点
    // 把“再见”等最终语义丢失。
    bool awaiting_final_asr_ = false;
    // 打断播报后，必须等待旧 TTS 的终止标记才能发送下一条确认播报。否则
    // 旧回合已经在传输中的 PCM 会被错误归入新 generation 而继续出声。
    bool interrupt_fence_pending_ = false;
    std::string pending_interrupt_wake_word_;
    std::string pending_interrupt_text_response_;
    // VAD 端点：本地静音检测（无 AFE，用 RMS 能量近似）。
    bool vad_speech_seen_ = false;
    bool vad_silence_emitted_ = false;
    bool vad_silence_pending_ = false;
    std::chrono::steady_clock::time_point last_speech_at_{};
    uint64_t generation_ = 0;
    uint64_t next_sequence_ = 0;
};

}  // namespace voicelife::voice
