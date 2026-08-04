#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::voice {

using VoiceEventSink = std::function<void(const VoiceEvent&)>;
using EvidenceSink = std::function<void(const VoiceEvidence&)>;
using AudioFrameSink = std::function<Status(AudioFrame)>;

/** 定义音频采集设备的生命周期边界。 */
class AudioInputPort {
   public:
    /** @brief 允许通过接口类型释放输入端口。 */
    virtual ~AudioInputPort() = default;
    /**
     * @brief 按协商格式打开输入设备。
     * @param format 输入音频格式。
     * @return 打开结果。
     */
    virtual Status Open(const AudioFormat& format) = 0;
    /**
     * @brief 开始指定模式的音频采集。
     * @param mode 采集触发模式。
     * @return 启动结果。
     */
    virtual Status StartCapture(VoiceMode mode) = 0;
    /**
     * @brief 停止当前音频采集。
     * @return 停止结果。
     */
    virtual Status StopCapture() = 0;
    /** @brief 关闭输入设备并释放其资源。 */
    virtual void Close() = 0;
};

/** 定义音频播放设备的生命周期和帧推送边界。 */
class AudioOutputPort {
   public:
    /** @brief 允许通过接口类型释放输出端口。 */
    virtual ~AudioOutputPort() = default;
    /**
     * @brief 按协商格式打开输出设备。
     * @param format 输出音频格式。
     * @return 打开结果。
     */
    virtual Status Open(const AudioFormat& format) = 0;
    /**
     * @brief 推送一帧待播放音频。
     * @param frame 待播放音频帧。
     * @return 推送结果。
     */
    virtual Status Push(const AudioFrame& frame) = 0;
    /**
     * @brief 等待已提交的播放数据完成。
     * @return 刷新结果。
     */
    virtual Status Flush() = 0;
    /** @brief 关闭输出设备并释放其资源。 */
    virtual void Close() = 0;
};

/** 定义语音会话与底层网络传输之间的协议无关边界。 */
class VoiceTransportPort {
   public:
    /** @brief 允许通过接口类型释放语音传输端口。 */
    virtual ~VoiceTransportPort() = default;
    /**
     * @brief 建立语音服务连接并注册事件回调。
     * @param config 会话连接配置。
     * @param sink 接收传输事件的回调。
     * @return 连接结果。
     */
    virtual Status Connect(const VoiceSessionConfig& config, VoiceEventSink sink) = 0;
    /**
     * @brief 发送文本控制消息。
     * @param message 待发送的协议消息。
     * @return 发送结果。
     */
    virtual Status SendText(std::string_view message) = 0;
    /**
     * @brief 发送一帧音频数据。
     * @param frame 待发送的音频帧。
     * @return 发送结果。
     */
    virtual Status SendAudio(const AudioFrame& frame) = 0;
    /**
     * @brief 关闭语音传输连接。
     * @return 关闭结果。
     */
    virtual Status Close() = 0;
};

/** 保留旧协调器生命周期接口，供迁移期 Adapter 逐步替换。 */
class SpeechProviderPort {
   public:
    /** @brief 允许通过接口类型释放旧 Provider 端口。 */
    virtual ~SpeechProviderPort() = default;
    /**
     * @brief 建立旧版语音服务连接。
     * @return 连接结果。
     */
    virtual Status Connect() = 0;
    /** @brief 断开旧版语音服务连接。 */
    virtual void Disconnect() = 0;
};

/** 定义音频编解码策略的统一接口。 */
class CodecStrategy {
   public:
    /** @brief 允许通过接口类型释放编解码策略。 */
    virtual ~CodecStrategy() = default;
    /**
     * @brief 返回该策略处理的编码格式。
     * @return 编码格式。
     */
    [[nodiscard]] virtual AudioCodec codec() const = 0;
    /**
     * @brief 将 PCM 音频编码为目标格式。
     * @param pcm 待编码的 PCM 帧。
     * @return 编码后的音频帧或错误。
     */
    virtual Result<AudioFrame> Encode(const AudioFrame& pcm) = 0;
    /**
     * @brief 将目标格式音频解码为 PCM。
     * @param encoded 待解码的音频帧。
     * @return 解码后的 PCM 帧或错误。
     */
    virtual Result<AudioFrame> Decode(const AudioFrame& encoded) = 0;
};

/** 将外部 STT 字段映射为稳定语音语义的协议防腐层。 */
class ASRAdapter {
   public:
    /** @brief 允许通过接口类型释放识别适配器。 */
    virtual ~ASRAdapter() = default;
    /**
     * @brief 消费一个语音领域事件。
     * @param event 待处理的语音事件。
     * @return 处理结果。
     */
    virtual Status OnEvent(const VoiceEvent& event) = 0;
};

/** 将外部 TTS 字段映射为稳定语音语义的协议防腐层。 */
class TTSAdapter {
   public:
    /** @brief 允许通过接口类型释放合成适配器。 */
    virtual ~TTSAdapter() = default;
    /**
     * @brief 请求合成并播放文本。
     * @param text 待合成文本。
     * @return 请求结果。
     */
    virtual Status Speak(std::string_view text) = 0;
    /**
     * @brief 消费一个语音领域事件。
     * @param event 待处理的语音事件。
     * @return 处理结果。
     */
    virtual Status OnEvent(const VoiceEvent& event) = 0;
};

/** 定义实时语音流的开始和中断边界。 */
class RealtimeAdapter {
   public:
    /** @brief 允许通过接口类型释放实时适配器。 */
    virtual ~RealtimeAdapter() = default;
    /**
     * @brief 开始指定模式的实时流。
     * @param mode 实时流模式。
     * @return 启动结果。
     */
    virtual Status Begin(VoiceMode mode) = 0;
    /**
     * @brief 中断当前实时流。
     * @return 中断结果。
     */
    virtual Status Interrupt() = 0;
};

/** 定义可插拔语音 Provider 的会话契约。 */
class SpeechProviderAdapter {
   public:
    /** @brief 允许通过接口类型释放 Provider 适配器。 */
    virtual ~SpeechProviderAdapter() = default;
    // Optional during migration. Providers with downlink audio should call
    // this sink for each decoded frame; the session owns generation checks.
    /**
     * @brief 设置 Provider 下行音频的接收回调。
     * @param sink 接收解码后音频帧的回调。
     */
    virtual void SetAudioSink(AudioFrameSink sink) { (void)sink; }
    // A single transport connection may survive an interrupt. The session
    // advances its epoch locally and gives the Provider the new epoch before
    // accepting the next stream.
    /**
     * @brief 设置当前会话代次以丢弃过期异步事件。
     * @param generation 当前会话代次。
     */
    virtual void SetGeneration(uint64_t generation) { (void)generation; }
    /**
     * @brief 建立 Provider 会话并注册事件回调。
     * @param config 语音会话配置。
     * @param sink 接收 Provider 事件的回调。
     * @return 连接结果。
     */
    virtual Status Connect(const VoiceSessionConfig& config, VoiceEventSink sink) = 0;
    /**
     * @brief 开始指定模式的采集。
     * @param mode 语音采集模式。
     * @return 启动结果。
     */
    virtual Status StartCapture(VoiceMode mode) = 0;
    /**
     * @brief 停止当前采集。
     * @return 停止结果。
     */
    virtual Status StopCapture() = 0;
    /**
     * @brief 发送一帧上行音频。
     * @param frame 待发送的音频帧。
     * @return 发送结果。
     */
    virtual Status SendAudio(const AudioFrame& frame) = 0;
    /**
     * @brief 中止当前识别或合成流程。
     * @param reason 中止原因。
     * @return 中止结果。
     */
    virtual Status Abort(std::string_view reason) = 0;
    /**
     * @brief 请求合成指定文本。
     * @param text 待合成文本。
     * @return 合成请求结果。
     */
    virtual Status Speak(std::string_view text) = 0;
    /**
     * @brief 断开 Provider 会话。
     * @return 断开结果。
     */
    virtual Status Disconnect() = 0;
    /**
     * @brief 返回 Provider 的能力声明。
     * @return 能力声明的只读引用。
     */
    [[nodiscard]] virtual const CapabilityProfile& capabilities() const = 0;
};

using SpeechProviderFactory = std::function<std::unique_ptr<SpeechProviderAdapter>()>;

/** 按 Provider 标识和能力声明创建适配器的进程内注册表。 */
class SpeechProviderRegistry {
   public:
    /** 注册表允许保存的 Provider 最大数量。 */
    static constexpr std::size_t kMaxProviders = 8;
    /**
     * @brief 返回进程内唯一的 Provider 注册表。
     * @return Provider 注册表实例。
     */
    static SpeechProviderRegistry& Instance();

    /**
     * @brief 注册一个可按能力选择的 Provider 工厂。
     * @param provider_id Provider 的稳定标识。
     * @param profile Provider 的能力声明。
     * @param factory 创建 Provider 实例的工厂。
     * @return 注册结果。
     */
    Status Register(std::string provider_id, CapabilityProfile profile, SpeechProviderFactory factory);
    /**
     * @brief 按标识和能力约束创建 Provider。
     * @param provider_id 目标 Provider 标识。
     * @param required_capabilities 必须满足的能力名称。
     * @return 新 Provider 实例或错误。
     */
    Result<std::unique_ptr<SpeechProviderAdapter>> Create(std::string_view provider_id,
                                                          const std::vector<std::string>& required_capabilities) const;

   private:
    SpeechProviderRegistry() = default;
    /** 保存一个 Provider 工厂及其能力声明。 */
    struct Entry {
        std::string provider_id;
        CapabilityProfile profile;
        SpeechProviderFactory factory;
    };
    std::array<Entry, kMaxProviders> entries_{};
    std::size_t size_ = 0;
};

}  // namespace voicelife::voice
