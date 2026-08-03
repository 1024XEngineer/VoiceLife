#pragma once

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
    /**
     * @brief 开始采集音频。
     * @return 开始结果。
     */
    Status BeginCapture();
    /**
     * @brief 结束采集音频。
     * @return 结束结果。
     */
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
     * @brief 请求 Provider 合成文本。
     * @param text 待合成文本。
     * @return 请求结果。
     */
    Status Speak(std::string_view text);
    /**
     * @brief 中断当前会话并推进会话代次。
     * @return 中断结果。
     */
    Status Interrupt();
    /**
     * @brief 停止会话并关闭所有音频资源。
     * @return 停止结果。
     */
    Status Stop();

    [[nodiscard]] VoiceSessionState state() const;
    [[nodiscard]] uint64_t generation() const;
    [[nodiscard]] VoiceSessionConfig config() const;

   private:
    void Emit(std::string_view event, std::string_view detail);
    void HandleEvent(const VoiceEvent& event);
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
    uint64_t generation_ = 0;
    uint64_t next_sequence_ = 0;
};

}  // namespace voicelife::voice
