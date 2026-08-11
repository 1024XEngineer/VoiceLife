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

/** @brief 语音事件回调：Provider 传输层将事件推送给会话。 */
using VoiceEventSink = std::function<void(const VoiceEvent&)>;

/** @brief 诊断证据回调：会话生命周期中产出可追踪事件。 */
using EvidenceSink = std::function<void(const VoiceEvidence&)>;

/** @brief 原始音频帧回调：Provider 下行或输入端口上行。 */
using AudioFrameSink = std::function<Status(AudioFrame)>;

/** @brief 硬件音频采集设备抽象（I2S 麦克风、AFE 管线等）。 */
class AudioInputPort {
   public:
    /** @brief 虚析构函数。 */
    virtual ~AudioInputPort() = default;

    /** @brief 设置采集帧回调。 @param sink 接收采集帧的回调。 */
    virtual void SetAudioSink(AudioFrameSink sink) = 0;

    /** @brief 按协商的上行格式打开采集设备。
     *  @param format 上行音频格式。
     *  @return 打开成功返回 Ok。 */
    virtual Status Open(const AudioFormat& format) = 0;

    /** @brief 开始向设置的回调投递采集帧。
     *  @param mode 采集模式。
     *  @return 启动成功返回 Ok。 */
    virtual Status StartCapture(VoiceMode mode) = 0;

    /** @brief 停止采集，此后迟到的帧会被会话拒绝。
     *  @return 停止成功返回 Ok。 */
    virtual Status StopCapture() = 0;

    /** @brief 释放硬件资源并清除回调。 */
    virtual void Close() = 0;
};

/** @brief 硬件音频播放设备抽象（I2S 扬声器、DAC 等）。 */
class AudioOutputPort {
   public:
    /** @brief 虚析构函数。 */
    virtual ~AudioOutputPort() = default;

    /** @brief 按协商的下行格式打开播放设备。
     *  @param format 下行音频格式。
     *  @return 打开成功返回 Ok。 */
    virtual Status Open(const AudioFormat& format) = 0;

    /** @brief 将解码后的音频帧推入播放队列。
     *  @param frame 要播放的音频帧。
     *  @return 推送成功返回 Ok。 */
    virtual Status Push(const AudioFrame& frame) = 0;

    /** @brief 丢弃所有缓冲帧，在打断或代次失效时调用。
     *  @return 刷新成功返回 Ok。 */
    virtual Status Flush() = 0;

    /** @brief 释放硬件资源。 */
    virtual void Close() = 0;
};

/** @brief 语音会话的低层传输端口（WebSocket、TCP 等）。 */
class VoiceTransportPort {
   public:
    /** @brief 虚析构函数。 */
    virtual ~VoiceTransportPort() = default;

    /** @brief 建立传输连接并注册事件/文本/音频回调。
     *  @param config 会话配置。
     *  @param sink 事件回调。
     *  @return 连接成功返回 Ok。 */
    virtual Status Connect(const VoiceSessionConfig& config, VoiceEventSink sink) = 0;

    /** @brief 通过传输发送文本控制消息。
     *  @param message 要发送的消息。
     *  @return 发送成功返回 Ok。 */
    virtual Status SendText(std::string_view message) = 0;

    /** @brief 通过传输发送音频帧。
     *  @param frame 要发送的音频帧。
     *  @return 发送成功返回 Ok。 */
    virtual Status SendAudio(const AudioFrame& frame) = 0;

    /** @brief 拆除传输连接。
     *  @return 关闭成功返回 Ok。 */
    virtual Status Close() = 0;
};

/** @brief 迁移期保留的旧生命周期端口，新代码应使用 SpeechProviderAdapter。 */
class SpeechProviderPort {
   public:
    /** @brief 虚析构函数。 */
    virtual ~SpeechProviderPort() = default;
    /** @brief 建立连接。 @return 成功返回 Ok。 */
    virtual Status Connect() = 0;
    /** @brief 断开连接。 */
    virtual void Disconnect() = 0;
};

/** @brief 特定音频编解码器的编解码策略（PCM、Opus 等）。 */
class CodecStrategy {
   public:
    /** @brief 虚析构函数。 */
    virtual ~CodecStrategy() = default;

    /** @brief 当前策略处理的编解码器类型。
     *  @return 编解码器枚举值。 */
    [[nodiscard]] virtual AudioCodec codec() const = 0;

    /** @brief 将 PCM 帧编码为压缩格式。
     *  @param pcm 原始 PCM 帧。
     *  @return 编码成功返回压缩帧。 */
    virtual Result<AudioFrame> Encode(const AudioFrame& pcm) = 0;

    /** @brief 将压缩帧解码回 PCM。
     *  @param encoded 压缩帧。
     *  @return 解码成功返回 PCM 帧。 */
    virtual Result<AudioFrame> Decode(const AudioFrame& encoded) = 0;
};

/** @brief 将 Provider 特有 ASR 事件映射为稳定 VoiceEvent 语义。 */
class ASRAdapter {
   public:
    /** @brief 虚析构函数。 */
    virtual ~ASRAdapter() = default;
    /** @brief 转发 ASR 事件。 @param event 语音事件。 @return 处理成功返回 Ok。 */
    virtual Status OnEvent(const VoiceEvent& event) = 0;
};

/** @brief 将 Provider 特有 TTS 事件映射为稳定 VoiceEvent 语义。 */
class TTSAdapter {
   public:
    /** @brief 虚析构函数。 */
    virtual ~TTSAdapter() = default;
    /** @brief 播报指定文本。 @param text 要播报的文本。 @return 成功返回 Ok。 */
    virtual Status Speak(std::string_view text) = 0;

    /** @brief 转发 TTS 事件。 @param event 语音事件。 @return 处理成功返回 Ok。 */
    virtual Status OnEvent(const VoiceEvent& event) = 0;
};

/** @brief 需要显式开始/打断信号的实时流协议适配器。 */
class RealtimeAdapter {
   public:
    /** @brief 虚析构函数。 */
    virtual ~RealtimeAdapter() = default;
    /** @brief 开始实时会话。 @param mode 会话模式。 @return 成功返回 Ok。 */
    virtual Status Begin(VoiceMode mode) = 0;
    /** @brief 打断当前操作。 @return 成功返回 Ok。 */
    virtual Status Interrupt() = 0;
};

/**
 * @brief 完整语音 Provider 抽象。
 *
 * 具体实现（Linx、xiaozhi 等）将传输、编解码器和协议逻辑封装
 * 在此单一接口之后。
 */
class SpeechProviderAdapter {
   public:
    /** @brief 虚析构函数。 */
    virtual ~SpeechProviderAdapter() = default;

    /**
     * @brief 设置下行音频回调。
     *
     * 迁移期可选。有下行音频的 Provider 应通过此回调投递每一解码帧，
     * 代次检查由会话层负责。
     * @param sink 接收下行音频帧的回调。
     */
    virtual void SetAudioSink(AudioFrameSink /*sink*/) {}

    /**
     * @brief 通知 Provider 当前连接代次。
     *
     * 旧代次的迟到帧会被拒绝。
     * @param generation 当前代次编号。
     */
    virtual void SetGeneration(uint64_t /*generation*/) {}

    /** @brief 建立 Provider 连接并注册事件回调。
     *  @param config 会话配置。
     *  @param sink 事件回调。
     *  @return 连接成功返回 Ok。 */
    virtual Status Connect(const VoiceSessionConfig& config, VoiceEventSink sink) = 0;

    /** @brief 在 Provider 侧启动上行音频采集。
     *  @param mode 采集模式。
     *  @return 启动成功返回 Ok。 */
    virtual Status StartCapture(VoiceMode mode) = 0;

    /** @brief 停止上行音频采集。 @return 停止成功返回 Ok。 */
    virtual Status StopCapture() = 0;

    /** @brief 向 Provider 发送音频帧。
     *  @param frame 要发送的音频帧。
     *  @return 发送成功返回 Ok。 */
    virtual Status SendAudio(const AudioFrame& frame) = 0;

    /** @brief 以指定原因中止当前操作（播放或采集）。
     *  @param reason 中止原因。
     *  @return 中止成功返回 Ok。 */
    virtual Status Abort(std::string_view reason) = 0;

    /** @brief 请求对指定文本进行 TTS 播报。
     *  @param text 要播报的文本。
     *  @return 请求成功返回 Ok。 */
    virtual Status Speak(std::string_view text) = 0;

    /** @brief 将本地唤醒词通知 Provider，启动一次云端语音轮次。
     *  @param wake_word 已由本地检测器确认的唤醒词。
     *  @return 通知发送结果。
     */
    virtual Status NotifyLocalWakeWord(std::string_view /*wake_word*/) {
        return Status::Error(ErrorCode::kUnavailable, "本地唤醒未实现");
    }

    /** @brief 拆除 Provider 连接。 @return 断开成功返回 Ok。 */
    virtual Status Disconnect() = 0;

    /**
     * @brief 返回服务端 hello 握手中协商的双向音频格式。
     *
     * 仅在 Connect() 完成后可用。
     * @return 协商后的双向音频格式。
     */
    [[nodiscard]] virtual Result<VoiceAudioFormats> audio_formats() const = 0;

    /** @brief 注册时声明的能力 Profile。
     *  @return 能力 Profile 引用。 */
    [[nodiscard]] virtual const CapabilityProfile& capabilities() const = 0;
};

/** @brief 创建 SpeechProviderAdapter 实例的工厂函数。 */
using SpeechProviderFactory = std::function<std::unique_ptr<SpeechProviderAdapter>()>;

/**
 * @brief 固定容量的 Provider 注册表。
 *
 * 所有 Register() 调用必须在 RTOS 调度器启动前完成；
 * 运行时通过 Create() 只读查询。
 *
 * @note 线程安全：内部无互斥锁，调用方负责初始化时序。
 */
class SpeechProviderRegistry {
   public:
    /** @brief 最大可注册 Provider 数量。 */
    static constexpr std::size_t kMaxProviders = 16;

    /** @brief 全局单例。 @return 注册表单例引用。 */
    static SpeechProviderRegistry& Instance();

    /**
     * @brief 注册 Provider 工厂及其能力 Profile。
     *
     * 必须在调度器启动前调用。
     * @param provider_id Provider 标识。
     * @param profile 能力 Profile。
     * @param factory 工厂函数。
     * @return 注册成功返回 Ok。
     */
    Status Register(std::string provider_id, CapabilityProfile profile, SpeechProviderFactory factory);

    /**
     * @brief 按 ID 和所需能力创建 Provider 实例。
     *
     * @param provider_id Provider 标识。
     * @param required_capabilities 所需能力列表。
     * @return 创建成功返回 Provider 实例。
     */
    Result<std::unique_ptr<SpeechProviderAdapter>> Create(std::string_view provider_id,
                                                          const std::vector<std::string>& required_capabilities) const;

   private:
    SpeechProviderRegistry() = default;
    /** @brief 注册表条目。 */
    struct Entry {
        std::string provider_id;
        CapabilityProfile profile;
        SpeechProviderFactory factory;
    };
    std::array<Entry, kMaxProviders> entries_{};
    std::size_t size_ = 0;
};

}  // namespace voicelife::voice
