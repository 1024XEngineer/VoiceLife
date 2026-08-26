#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/display_snapshot.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::voice {

/** @brief 语音事件回调：Provider 传输层将事件推送给会话。 */
using VoiceEventSink = std::function<void(const VoiceEvent&)>;

/** @brief 诊断证据回调：会话生命周期中产出可追踪事件。 */
using EvidenceSink = std::function<void(const VoiceEvidence&)>;

/** @brief 原始音频帧回调：Provider 下行或输入端口上行。 */
using AudioFrameSink = std::function<Status(AudioFrame)>;

/**
 * @brief 显示能力和资源上限，不暴露图形框架或板级总线细节。
 *
 * 语义约束（方案 A）：available 表示当前渲染路径是否可用；available 为
 * false 时，text/static_image/animation/preview_image 必须全部为 false，
 * 调用方不得据此分配或声明资源。max_frame_bytes 与 refresh_budget_hz 是
 * 硬件上限参考（而非当前 Renderer 已验证值）：在官方 Renderer 移植与真机
 * 证据完成前，Runtime 不得使用这两个字段做真实资源分配。
 */
struct DisplayCapabilities {
    /** @brief 当前渲染路径是否可用（Renderer/资源就绪后为 true）。 */
    bool available = false;
    /** @brief 是否支持文本（仅 available 为 true 时有效）。 */
    bool text = false;
    /** @brief 是否支持静态图片（仅 available 为 true 时有效）。 */
    bool static_image = false;
    /** @brief 是否支持动画（仅 available 为 true 时有效）。 */
    bool animation = false;
    /** @brief 是否支持受约束的预览图（仅 available 为 true 时有效）。 */
    bool preview_image = false;
    /** @brief 单帧缓冲硬件上限（字节），非当前可用值。 */
    uint32_t max_frame_bytes = 0;
    /** @brief 刷新预算硬件上限（Hz），未核实前为 0。 */
    uint32_t refresh_budget_hz = 0;
};

/**
 * @brief 业务语义到板级显示实现的端口。
 *
 * 调用上下文契约：
 * - 唯一提交者是交互事件循环（InteractionEventLoop 或等效的显示快照生产
 *   者）；Provider 回调、音频实时任务、输入回调和定时器不得直接调用
 *   Render，只能投递事件。
 * - Render 必须在显示适配器专属的显示任务/受控上下文内执行，由
 *   Adapter 负责串行化、丢弃旧 revision/旧 generation 并防止阻塞音频任务。
 * - LVGL 对象、GIF 解码、缓存、路径和像素缓冲区只存在于具体显示 Adapter
 *   的专属上下文中，不反向暴露给 Runtime 或 Domain。
 * 调用方不能传入 URL 或任意文件路径。
 */
class PresentationPort {
   public:
    /** @brief 虚析构函数。 */
    virtual ~PresentationPort() = default;

    /** @brief 返回当前显示适配器声明的能力和资源预算。 @return 显示能力引用。 */
    [[nodiscard]] virtual const DisplayCapabilities& capabilities() const = 0;

    /**
     * @brief 提交一份完整显示快照。
     * @param snapshot 只包含业务语义的显示快照。
     * @return 快照被接受或明确失败时的状态。
     */
    virtual Status Render(const DisplaySnapshot& snapshot) = 0;
};

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

    /** @brief 丢弃采集端已经排队的 PCM，建立新的语音回合边界。
     *
     * 默认端口没有独立的有界输入队列时无需操作；硬件端口应在自身
     * 队列锁内清除排队帧及未完成组帧状态。
     * @return 清理成功返回 Ok。
     */
    virtual Status DiscardPendingInput() { return Status::Ok(); }

    /** @brief 释放硬件资源并清除回调。 */
    virtual void Close() = 0;
};

/**
 * @brief 仅供受控硬件回归使用的采集注入端口。
 *
 * 实现必须把帧送入与物理采集相同的有界投递路径。正常产品流程不依赖此
 * 端口；没有测试注入能力的平台返回空指针即可。
 */
class TestAudioInjectionPort {
   public:
    /** @brief 虚析构函数。 */
    virtual ~TestAudioInjectionPort() = default;

    /** @brief 启用或关闭测试输入对物理麦克风上行的排他保护。
     * @param enabled 为 true 时由测试输入独占上行。
     * @return 状态切换成功返回 Ok。
     */
    virtual Status SetTestInputEnabled(bool enabled) = 0;
    /** @brief 提交一帧已协商格式的测试 PCM。
     * @param frame 要注入的音频帧，调用方交出负载所有权。
     * @return 帧被接收或拒绝时对应的状态。
     */
    virtual Status InjectTestInput(AudioFrame frame) = 0;
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

    /** @brief 将解码后的音频帧移入播放队列。
     *  @param frame 要播放的音频帧，调用方交出其负载所有权。
     *  @return 推送成功返回 Ok。 */
    virtual Status Push(AudioFrame frame) = 0;

    /** @brief 丢弃所有缓冲帧，在打断或代次失效时调用。
     *  @return 刷新成功返回 Ok。 */
    virtual Status Flush() = 0;

    /** @brief 查询播放队列是否已排空（无待播帧）。
     *  @return true 表示播放已排空，可用于 TTS 结束后的收尾判断。 */
    [[nodiscard]] virtual bool IsIdle() const = 0;

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
     *  @param frame 要发送的音频帧，调用方交出其负载所有权。
     *  @return 发送成功返回 Ok。 */
    virtual Status SendAudio(AudioFrame frame) = 0;

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

    /**
     * @brief 配置本地 PCM 与线上编码格式的固定转换契约。
     * @param local_pcm 本地采集或播放的 PCM 格式。
     * @param wire 线上传输的编码格式。
     * @return 配置成功返回 Ok，否则返回不支持的格式或资源错误。
     *
     * Provider 必须在 hello 之前调用此方法，使资源、帧时长或码率不满足时
     * 在宣告线上格式前失败，而不是在第一帧语音到达时才暴露。
     */
    virtual Status Configure(const AudioFormat& local_pcm, const AudioFormat& wire) = 0;

    /** @brief 将 PCM 帧编码为压缩格式。
     *  @param pcm 原始 PCM 帧。
     *  @return 编码成功返回压缩帧。 */
    virtual Result<AudioFrame> Encode(const AudioFrame& pcm) = 0;

    /** @brief 将压缩帧解码回 PCM。
     *  @param encoded 压缩帧。
     *  @return 解码成功返回 PCM 帧；固定容量池的可恢复满载必须返回 kConflict，
     *          由 Provider 丢弃该帧而不改变连接生命周期。 */
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
    virtual Status SendAudio(AudioFrame frame) = 0;

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
    virtual Status NotifyLocalWakeWord(std::string_view /*wake_word*/, std::string_view /*text_response*/ = {}) {
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
