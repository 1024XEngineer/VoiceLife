#pragma once

#include <cstddef>
#include <cstdint>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::voice {

/** @brief AEC 输入格式和有界实时缓冲配置。 */
struct AudioAecConfig {
    /** @brief 麦克风采集 PCM 格式。 */
    AudioFormat capture_format;
    /** @brief 实际送入扬声器链路的 playback reference 格式。 */
    AudioFormat playback_reference_format;
    /** @brief 回声尾长，交给底层 AEC 实现使用。 */
    uint32_t tail_length_ms = 200;
    /** @brief 采集与 reference 队列的最大帧数。 */
    std::size_t queue_depth_frames = 4;
    /** @brief 与 VoiceSession 对齐的连接代次。 */
    uint64_t generation = 0;
};

/** @brief AEC 能力声明；available 为 false 时不得宣称已消除回声。 */
struct AudioAecCapabilities {
    bool available = false;
    bool playback_reference = false;
    bool full_duplex = false;
};

/** @brief AEC 服务运行统计，用于定位实时路径丢帧和代次污染。 */
struct AudioAecStats {
    uint64_t capture_frames = 0;
    uint64_t reference_frames = 0;
    uint64_t processed_frames = 0;
    uint64_t dropped_capture_frames = 0;
    uint64_t rejected_reference_frames = 0;
    uint64_t stale_generation_frames = 0;
};

/**
 * @brief 校验 AEC 配置的跨平台约束。
 *
 * AEC 必须使用同采样率、同帧时长的 PCM S16LE capture/reference（声道映射由
 * 平台适配器按硬件 wiring 处理）；重采样和
 * Codec 解码属于 Audio Adapter，不应在 AEC 实时任务中隐式分配临时缓冲。
 * @param config 待校验的 AEC 配置。
 * @return 格式、队列和尾长均合法时返回成功。
 */
[[nodiscard]] Status ValidateAudioAecConfig(const AudioAecConfig& config);

/**
 * @brief 平台无关 AEC 服务契约。
 *
 * ESP-SR/AEC 句柄、任务和事件对象只能由具体适配器拥有。实现必须保证
 * SubmitCapture 不等待慢网络或播放消费者，且必须消费真实 playback reference。
 */
class AudioAecService {
   public:
    /** @brief 允许通过接口类型释放 AEC 服务。 */
    virtual ~AudioAecService() = default;

    /** @brief 返回当前硬件和实现可证明的 AEC 能力。 @return 能力快照。 */
    [[nodiscard]] virtual AudioAecCapabilities capabilities() const = 0;
    /**
     * @brief 校验配置并启动 AEC 处理。
     * @param config 本轮 capture/reference 和 generation 配置。
     * @param cleaned_sink 接收清理后采集 PCM 的非空回调。
     * @return 启动结果。
     */
    virtual Status Start(const AudioAecConfig& config, AudioFrameSink cleaned_sink) = 0;
    /** @brief 非阻塞提交麦克风采集帧。 @param frame 当前 generation 的 PCM 帧。 @return 接收结果。 */
    virtual Status SubmitCapture(AudioFrame frame) = 0;
    /** @brief 提交真实播放链路的 reference 帧。 @param frame 当前 generation 的 PCM 帧。 @return 接收结果。 */
    virtual Status SubmitPlaybackReference(AudioFrame frame) = 0;
    /** @brief 请求启用或旁路 AEC。 @param enabled 为 true 时处理 capture。 @return 设置结果。 */
    virtual Status SetEnabled(bool enabled) = 0;
    /** @brief 失效旧帧并重置处理状态。 @param generation 新连接代次。 @return 重置结果。 */
    virtual Status Reset(uint64_t generation) = 0;
    /** @brief 停止处理并释放本轮回调与缓冲。 @return 停止结果。 */
    virtual Status Stop() = 0;
    /** @brief 返回累计处理统计。 @return 统计快照。 */
    [[nodiscard]] virtual AudioAecStats stats() const = 0;
};

}  // namespace voicelife::voice
