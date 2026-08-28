#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace voicelife::audio_esp {

/**
 * @brief 仅用于本地 MultiNet 输入的有界音量调节参数。
 *
 * 硬件采集路径保持不变；在不同麦克风和 TTS 评估音频间稳定唤醒灵敏度，
 * 同时限制安静噪声的放大量。
 */
struct MultiNetAudioConditioningConfig {
    uint32_t noise_floor_rms = 320;
    uint32_t target_rms = 2800;
    uint16_t min_gain_q10 = 768;   // 0.75x
    uint16_t max_gain_q10 = 4096;  // 4.0x
    uint16_t max_peak = 30000;
};

/** @brief 一帧 MultiNet 输入调节前后的测量结果。 */
struct MultiNetAudioConditioningStats {
    uint32_t input_rms = 0;
    uint32_t output_rms = 0;
    uint16_t input_peak = 0;
    uint16_t output_peak = 0;
    uint16_t gain_q10 = 1024;
    bool gated = false;
};

inline constexpr MultiNetAudioConditioningConfig kDefaultMultiNetAudioConditioning{};

namespace detail {

/**
 * @brief 计算无符号整数的平方根。
 * @param value 非负被开方数。
 * @return 向下取整后的平方根。
 */
inline uint32_t IntegerSqrt(uint64_t value) {
    uint64_t bit = 1ULL << 62;
    while (bit > value) bit >>= 2;
    uint64_t result = 0;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<uint32_t>(result);
}

/**
 * @brief 返回 16 位 PCM 样本的绝对幅度。
 * @param sample 输入 PCM 样本。
 * @return 不会溢出的无符号幅度。
 */
inline uint16_t AbsolutePcm16(int16_t sample) {
    const int32_t value = sample;
    return static_cast<uint16_t>(value < 0 ? -value : value);
}

}  // namespace detail

/**
 * @brief 原地执行有界 RMS 调节并返回测量结果。
 * @param samples 待调节的 PCM 样本数组；可为空。
 * @param sample_count 样本数量。
 * @param config 噪声门、目标 RMS、增益和峰值限制。
 * @return 调节前后 RMS、峰值、增益和噪声门状态。
 */
inline MultiNetAudioConditioningStats ConditionMultiNetPcm(
    int16_t* samples, std::size_t sample_count,
    const MultiNetAudioConditioningConfig& config = kDefaultMultiNetAudioConditioning) {
    MultiNetAudioConditioningStats stats;
    if (samples == nullptr || sample_count == 0) {
        stats.gated = true;
        return stats;
    }

    uint64_t sum_squares = 0;
    uint16_t peak = 0;
    for (std::size_t index = 0; index < sample_count; ++index) {
        const int32_t sample = samples[index];
        sum_squares += static_cast<uint64_t>(static_cast<int64_t>(sample) * sample);
        peak = std::max(peak, detail::AbsolutePcm16(samples[index]));
    }
    stats.input_rms = detail::IntegerSqrt(sum_squares / sample_count);
    stats.input_peak = peak;
    if (stats.input_rms < config.noise_floor_rms) {
        stats.gated = true;
        return stats;
    }

    const uint32_t requested_gain =
        (config.target_rms * static_cast<uint32_t>(1024)) / std::max(stats.input_rms, static_cast<uint32_t>(1));
    uint32_t gain = std::clamp(requested_gain, static_cast<uint32_t>(config.min_gain_q10),
                               static_cast<uint32_t>(config.max_gain_q10));
    if (peak != 0) {
        const uint32_t peak_limited_gain = (static_cast<uint32_t>(config.max_peak) * 1024U) / peak;
        gain = std::min(gain, peak_limited_gain);
    }
    stats.gain_q10 = static_cast<uint16_t>(std::min(gain, static_cast<uint32_t>(UINT16_MAX)));

    uint64_t output_sum_squares = 0;
    uint16_t output_peak = 0;
    for (std::size_t index = 0; index < sample_count; ++index) {
        const int32_t scaled = static_cast<int32_t>(samples[index]) * static_cast<int32_t>(gain) / 1024;
        samples[index] = static_cast<int16_t>(
            std::clamp<int32_t>(scaled, static_cast<int32_t>(-32768), static_cast<int32_t>(32767)));
        output_sum_squares += static_cast<uint64_t>(static_cast<int64_t>(samples[index]) * samples[index]);
        output_peak = std::max(output_peak, detail::AbsolutePcm16(samples[index]));
    }
    stats.output_rms = detail::IntegerSqrt(output_sum_squares / sample_count);
    stats.output_peak = output_peak;
    return stats;
}

}  // namespace voicelife::audio_esp
