#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "voicelife/voice/voice_ports.h"

namespace voicelife::linx {

/** 保存 Linx WebSocket 连接所需的非敏感配置引用。 */
struct LinxConnectionConfig {
    std::string websocket_url;
    // A reference such as secret://linx/device-token. The resolved token is
    // owned by the platform transport and never enters this component's logs.
    std::string token_ref;
    std::string device_id;
    std::string client_id;
    std::optional<std::string> agent_id;

    /**
     * @brief 校验连接配置是否完整。
     * @return 必需字段均非空时返回 true。
     */
    [[nodiscard]] bool valid() const {
        return !websocket_url.empty() && !token_ref.empty() && !device_id.empty() && !client_id.empty();
    }
};

/** 描述 Linx 协议协商出的音频参数。 */
struct LinxAudioParams {
    voice::AudioCodec codec = voice::AudioCodec::kPcmS16Le;
    uint32_t sample_rate_hz = 16000;
    uint8_t channels = 1;
    uint8_t bits_per_sample = 16;
    uint16_t frame_duration_ms = 20;

    /**
     * @brief 校验音频协商参数是否有效。
     * @return 参数有效时返回 true。
     */
    [[nodiscard]] bool valid() const {
        return sample_rate_hz > 0 && channels > 0 && bits_per_sample > 0 && frame_duration_ms > 0;
    }
};

/** 表示 Linx 入站控制消息的类别。 */
enum class LinxMessageKind { kHello, kStt, kTts, kError };
/** 表示 Linx TTS 消息的生命周期状态。 */
enum class LinxTtsState { kStart, kSentenceStart, kStop };

/** 保存解码后的 Linx 入站消息语义。 */
struct LinxInboundMessage {
    LinxMessageKind kind = LinxMessageKind::kError;
    std::optional<std::string> session_id;
    std::optional<LinxAudioParams> audio_params;
    std::optional<LinxTtsState> tts_state;
    std::string text;
    bool aborted = false;
};

/** 保存 Linx 传输层向 Provider 上报事件的回调集合。 */
struct LinxTransportSink {
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void(std::string_view)> on_text;
    std::function<void(const std::vector<uint8_t>&)> on_binary;
    std::function<void(Status)> on_error;
};

/** 定义 Linx WebSocket 传输的异步端口。 */
class LinxTransportPort {
   public:
    /** @brief 允许通过接口类型释放传输端口。 */
    virtual ~LinxTransportPort() = default;
    /**
     * @brief 建立 Linx 连接并注册事件回调。
     * @param config Linx 连接配置。
     * @param sink 传输事件回调集合。
     * @return 连接结果。
     */
    virtual Status Connect(const LinxConnectionConfig& config, LinxTransportSink sink) = 0;
    /**
     * @brief 发送 Linx 文本控制消息。
     * @param message 已编码的文本消息。
     * @return 发送结果。
     */
    virtual Status SendText(std::string_view message) = 0;
    /**
     * @brief 发送一帧音频数据。
     * @param frame 待发送音频帧。
     * @return 发送结果。
     */
    virtual Status SendAudio(const voice::AudioFrame& frame) = 0;
    /**
     * @brief 关闭 Linx 连接。
     * @return 关闭结果。
     */
    virtual Status Close() = 0;
    virtual void SetGeneration(uint64_t) {}
};

/** 定义 Linx 控制消息与领域语义之间的编解码端口。 */
class LinxProtocolCodecPort {
   public:
    /** @brief 允许通过接口类型释放协议编解码端口。 */
    virtual ~LinxProtocolCodecPort() = default;
    /**
     * @brief 编码 hello 握手消息。
     * @param config 语音会话配置。
     * @param connection Linx 连接配置。
     * @return 已编码消息或错误。
     */
    virtual Result<std::string> EncodeHello(const voice::VoiceSessionConfig& config,
                                            const LinxConnectionConfig& connection) const = 0;
    /**
     * @brief 编码开始监听消息。
     * @param config 语音会话配置。
     * @return 已编码消息或错误。
     */
    virtual Result<std::string> EncodeListenStart(const voice::VoiceSessionConfig& config) const = 0;
    /**
     * @brief 编码停止监听消息。
     * @param config 语音会话配置。
     * @return 已编码消息或错误。
     */
    virtual Result<std::string> EncodeListenStop(const voice::VoiceSessionConfig& config) const = 0;
    /**
     * @brief 编码检测文本消息。
     * @param config 语音会话配置。
     * @param text 要发送的文本。
     * @return 已编码消息或错误。
     */
    virtual Result<std::string> EncodeListenDetect(const voice::VoiceSessionConfig& config,
                                                   std::string_view text) const = 0;
    /**
     * @brief 编码中止消息。
     * @param config 语音会话配置。
     * @param reason 中止原因。
     * @return 已编码消息或错误。
     */
    virtual Result<std::string> EncodeAbort(const voice::VoiceSessionConfig& config, std::string_view reason) const = 0;
    /**
     * @brief 解码 Linx 文本消息。
     * @param message 收到的协议文本。
     * @return 入站消息语义或错误。
     */
    virtual Result<LinxInboundMessage> DecodeText(std::string_view message) const = 0;
};

/** 使用受限 JSON 形状实现的可移植 Linx 编解码器。 */
class LinxJsonCodec final : public LinxProtocolCodecPort {
   public:
    /**
     * @brief 编码 Linx hello 握手消息。
     * @param config 语音会话配置。
     * @param connection Linx 连接配置。
     * @return 已编码消息或错误。
     */
    Result<std::string> EncodeHello(const voice::VoiceSessionConfig& config,
                                    const LinxConnectionConfig& connection) const override;
    /**
     * @brief 编码 Linx 开始监听消息。
     * @param config 语音会话配置。
     * @return 已编码消息或错误。
     */
    Result<std::string> EncodeListenStart(const voice::VoiceSessionConfig& config) const override;
    /**
     * @brief 编码 Linx 停止监听消息。
     * @param config 语音会话配置。
     * @return 已编码消息或错误。
     */
    Result<std::string> EncodeListenStop(const voice::VoiceSessionConfig& config) const override;
    /**
     * @brief 编码 Linx 文本检测消息。
     * @param config 语音会话配置。
     * @param text 要发送的文本。
     * @return 已编码消息或错误。
     */
    Result<std::string> EncodeListenDetect(const voice::VoiceSessionConfig& config,
                                           std::string_view text) const override;
    /**
     * @brief 编码 Linx 中止消息。
     * @param config 语音会话配置。
     * @param reason 中止原因。
     * @return 已编码消息或错误。
     */
    Result<std::string> EncodeAbort(const voice::VoiceSessionConfig& config, std::string_view reason) const override;
    /**
     * @brief 解码 Linx 文本消息为领域语义。
     * @param message 收到的协议文本。
     * @return 入站消息语义或错误。
     */
    Result<LinxInboundMessage> DecodeText(std::string_view message) const override;
};

}  // namespace voicelife::linx
