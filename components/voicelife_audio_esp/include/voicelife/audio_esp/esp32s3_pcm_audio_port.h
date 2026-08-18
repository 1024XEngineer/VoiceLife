#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::audio_esp {

/** @brief 音频端口选项。 */
struct AudioPortOptions {
    /** @brief I/O 超时毫秒数。 */
    uint32_t io_timeout_ms = 100;
    /** @brief 输入队列深度。 */
    std::size_t input_queue_depth = 4;
    /** @brief 输出队列深度。 */
    std::size_t output_queue_depth = 8;
    /** @brief 名义播放队列允许的最大延迟（毫秒）。 */
    uint32_t maximum_playback_latency_ms = 500;
};

/** @brief 音频端口统计。 */
struct AudioPortStats {
    /** @brief 采集帧数。 */
    std::size_t captured_frames = 0;
    /** @brief 丢弃的输入帧数。 */
    std::size_t dropped_input_frames = 0;
    /** @brief 播放帧数。 */
    std::size_t played_frames = 0;
    /** @brief 拒绝的输出帧数。 */
    std::size_t rejected_output_frames = 0;
    /** @brief 短读次数。 */
    std::size_t short_reads = 0;
    /** @brief 短写次数。 */
    std::size_t short_writes = 0;
    /** @brief 输入队列高水位。 */
    std::size_t input_high_watermark = 0;
    /** @brief 输出队列高水位。 */
    std::size_t output_high_watermark = 0;
    /** @brief 最小空闲堆字节数。 */
    std::size_t minimum_free_heap_bytes = 0;
    /** @brief 已从 I2S 转换的输入 PCM 字节数。 */
    uint64_t input_pcm_bytes = 0;
    /** @brief 已提交给 I2S 的输出 PCM 字节数。 */
    uint64_t output_pcm_bytes = 0;
    /** @brief 输入 PCM 样本数与平方和（用于离线推导 RMS）。 */
    uint64_t input_samples = 0;
    uint64_t input_sum_squares = 0;
    /** @brief 输出缩放后 PCM 样本数与平方和（用于离线推导 RMS）。 */
    uint64_t output_samples = 0;
    uint64_t output_sum_squares = 0;
    /** @brief 输入/输出绝对峰值。 */
    uint16_t input_peak = 0;
    uint16_t output_peak = 0;
    /** @brief 完全静音（所有样本为零）的 PCM period 数。 */
    uint64_t input_zero_periods = 0;
    uint64_t output_zero_periods = 0;
    /** @brief 数字音量缩放导致的削波样本数。 */
    uint64_t output_clipped_samples = 0;
    /** @brief I2S API 返回错误次数（短读/短写另计）。 */
    uint64_t input_i2s_errors = 0;
    uint64_t output_i2s_errors = 0;
    /** @brief 当前生效的板端输出音量。 */
    uint8_t output_volume = 0;
};

/**
 * @brief 持有 Profile 驱动的 RX/TX 通道对，并暴露两个平台无关 Port。
 *
 * 共享所有者对全双工 Codec Profile 很重要：两个通道必须作为一个
 * 硬件资源统一初始化和释放。
 */
class Esp32s3PcmAudioPorts final {
   public:
    /** @brief 功放请求回调（经板级仲裁，不得直接写 GPIO）。 */
    using AmplifierCallback = std::function<void(bool)>;

   public:
    /**
     * @brief 构造音频端口。
     * @param profile 音频板 Profile。
     * @param options 端口选项。
     * @param amplifier_callback 可选功放请求回调（经板级仲裁）。
     */
    Esp32s3PcmAudioPorts(AudioBoardProfile profile, AudioPortOptions options = {},
                         AmplifierCallback amplifier_callback = {});
    /** @brief 析构音频端口。 */
    ~Esp32s3PcmAudioPorts();

    /** @brief 禁止拷贝构造。 */
    Esp32s3PcmAudioPorts(const Esp32s3PcmAudioPorts&) = delete;
    /** @brief 禁止拷贝赋值。 */
    Esp32s3PcmAudioPorts& operator=(const Esp32s3PcmAudioPorts&) = delete;

    /** @brief 采集输入端口。 @return 平台无关 AudioInputPort 引用。 */
    [[nodiscard]] voice::AudioInputPort& input();
    /** @brief 播放输出端口。 @return 平台无关 AudioOutputPort 引用。 */
    [[nodiscard]] voice::AudioOutputPort& output();
    /** @brief 端口统计。 @return 统计值。 */
    [[nodiscard]] AudioPortStats stats() const;
    /** @brief 设置板端 PCM 播放音量（0-100）。 @param volume 目标音量百分比。 */
    void SetOutputVolume(uint8_t volume);
    /** @brief 返回当前板端 PCM 播放音量。 @return 当前音量百分比。 */
    [[nodiscard]] uint8_t output_volume() const;

   private:
    /** @brief Pimpl 实现。 */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::audio_esp
