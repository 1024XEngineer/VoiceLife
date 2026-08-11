#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "voicelife/audio_esp/audio_board_profile.h"

namespace voicelife::audio_esp {

/** @brief 板级探针报告：I2C/I2S/Codec 探测与采集统计。 */
struct AudioProbeReport {
    /** @brief 是否需要 Codec 控制。 */
    bool codec_control_required = false;
    /** @brief I2C 总线是否就绪。 */
    bool i2c_bus_ready = false;
    /** @brief ES8311 ACK。 */
    bool es8311_ack = false;
    /** @brief ES7210 ACK。 */
    bool es7210_ack = false;
    /** @brief PCA9557 ACK。 */
    bool pca9557_ack = false;
    /** @brief I2S 通道是否就绪。 */
    bool i2s_channels_ready = false;
    /** @brief I2S 通道是否已启动。 */
    bool i2s_channels_started = false;
    /** @brief 写入字节数。 */
    std::size_t bytes_written = 0;
    /** @brief 读取字节数。 */
    std::size_t bytes_read = 0;
    /** @brief 回放写入字节数。 */
    std::size_t replay_bytes_written = 0;
    /** @brief 1kHz 正弦探针写入字节数。 */
    std::size_t probe_tone_written = 0;
    /** @brief 1kHz 正弦探针是否完整写入。 */
    bool probe_tone_ok = false;
    /** @brief 采集样本数。 */
    std::size_t capture_samples = 0;
    /** @brief 非零样本数。 */
    std::size_t nonzero_samples = 0;
    /** @brief 变化样本数。 */
    std::size_t changed_samples = 0;
    /** @brief 饱和样本数。 */
    std::size_t saturated_samples = 0;
    /** @brief 峰值绝对值。 */
    uint32_t peak_abs = 0;
    /** @brief 样本平方和。 */
    uint64_t sum_squares = 0;
    /** @brief 运行期间最小空闲堆字节数。 */
    std::size_t minimum_free_heap_bytes = 0;

    /** @brief 硬件是否就绪（Codec + I2S 全部满足）。 @return 就绪返回 true。 */
    [[nodiscard]] bool hardware_ready() const {
        const bool codec_ready = !codec_control_required || (i2c_bus_ready && es8311_ack && es7210_ack && pca9557_ack);
        return codec_ready && i2s_channels_ready && i2s_channels_started;
    }

    /** @brief 均方值（信号能量估计）。 @return 无采集样本时返回 0。 */
    [[nodiscard]] uint64_t mean_square() const { return capture_samples == 0 ? 0 : sum_squares / capture_samples; }

    /** @brief 饱和比例（ppm）。 @return 无采集样本时返回 0。 */
    [[nodiscard]] uint64_t saturation_ratio_ppm() const {
        return capture_samples == 0 ? 0 : saturated_samples * 1000000ULL / capture_samples;
    }

    /** @brief 是否检测到采集信号。
     *  仅限数字输入证据，不声称扬声器被听到；真实听感需外部声学观察者。
     *  @return 检测到信号返回 true。 */
    [[nodiscard]] bool capture_signal_detected() const {
        const uint64_t msq = mean_square();
        return capture_samples >= 160 && nonzero_samples >= capture_samples / 20 &&
               changed_samples >= capture_samples / 100 && peak_abs >= 32 && msq >= 256;
    }
};

/** @brief 板级探针选项。 */
struct AudioProbeOptions {
    /** @brief 超时毫秒数。 */
    uint32_t timeout_ms = 200;
    /** @brief 采集时长毫秒数。 */
    uint32_t capture_duration_ms = 300;
    /** @brief 是否回放采集帧。 */
    bool replay_capture = false;
};

/**
 * @brief 有界的板级探针。
 *
 * 验证选定拓扑、I2S DMA 生命周期、逻辑 PCM 证据，以及（可选）一次
 * 衰减回放帧。不初始化 Codec 寄存器；无外部观察者时不声称声学播放。
 */
class Esp32s3AudioProbe final {
   public:
    /** @brief 构造探针。 */
    Esp32s3AudioProbe();
    /** @brief 析构探针。 */
    ~Esp32s3AudioProbe();

    /** @brief 禁止拷贝构造。 */
    Esp32s3AudioProbe(const Esp32s3AudioProbe&) = delete;
    /** @brief 禁止拷贝赋值。 */
    Esp32s3AudioProbe& operator=(const Esp32s3AudioProbe&) = delete;

    /**
     * @brief 运行板级探针。
     * @param profile 音频板 Profile。
     * @param options 探针选项。
     * @return 探针报告。
     */
    Result<AudioProbeReport> Run(const AudioBoardProfile& profile, const AudioProbeOptions& options = {});

   private:
    /** @brief Pimpl 实现。 */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::audio_esp
