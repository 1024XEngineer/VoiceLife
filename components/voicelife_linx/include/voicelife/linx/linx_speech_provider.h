#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "voicelife/linx/linx_types.h"

namespace voicelife::linx {

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
                              voice::CapabilityProfile capabilities = DefaultCapabilities());

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
    /**
     * @brief 开始指定模式的音频采集。
     * @param mode 采集模式。
     * @return 启动结果。
     */
    Status StartCapture(voice::VoiceMode mode) override;
    /**
     * @brief 停止当前音频采集。
     * @return 停止结果。
     */
    Status StopCapture() override;
    /**
     * @brief 发送一帧上行音频。
     * @param frame 待发送的音频帧。
     * @return 发送结果。
     */
    Status SendAudio(const voice::AudioFrame& frame) override;
    /**
     * @brief 中止当前 Linx 会话。
     * @param reason 中止原因。
     * @return 中止结果。
     */
    Status Abort(std::string_view reason) override;
    /**
     * @brief 请求 Linx 合成文本。
     * @param text 待合成文本。
     * @return 请求结果。
     */
    Status Speak(std::string_view text) override;
    /**
     * @brief 断开 Linx Provider 会话。
     * @return 断开结果。
     */
    Status Disconnect() override;
    /**
     * @brief 返回 Linx Provider 能力声明。
     * @return 能力声明的只读引用。
     */
    [[nodiscard]] const voice::CapabilityProfile& capabilities() const override { return capabilities_; }

    /**
     * @brief 返回 Linx Provider 的默认能力集合。
     * @return 默认能力声明。
     */
    static voice::CapabilityProfile DefaultCapabilities();

   private:
    void OnText(std::string_view message);
    void OnBinary(const std::vector<uint8_t>& payload);
    Status Send(Result<std::string> encoded);
    void Emit(voice::VoiceEvent event);

    LinxTransportPort& transport_;
    LinxProtocolCodecPort& codec_;
    LinxConnectionConfig connection_;
    voice::CapabilityProfile capabilities_;
    voice::VoiceSessionConfig config_;
    voice::VoiceEventSink event_sink_;
    voice::AudioFrameSink audio_sink_;
    uint64_t generation_ = 0;
    uint64_t output_sequence_ = 0;
    bool connected_ = false;
    mutable std::mutex hello_mutex_;
    std::condition_variable hello_cv_;
    bool hello_received_ = false;
    Status hello_status_ = Status::Ok();
};

}  // namespace voicelife::linx
