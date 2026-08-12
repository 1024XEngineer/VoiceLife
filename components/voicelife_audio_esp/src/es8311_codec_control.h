#pragma once

#include <cstddef>
#include <cstdint>

#include "voicelife/contracts/status.h"

namespace voicelife::audio_esp {

/** @brief ES8311 控制面初始化参数（官方 esp_codec_dev 组件方式）。 */
struct Es8311ControlConfig {
    /** @brief I2C 控制器编号。 */
    int i2c_port = 0;
    /** @brief SDA GPIO。 */
    int sda_gpio = -1;
    /** @brief SCL GPIO。 */
    int scl_gpio = -1;
    /** @brief ES8311 8-bit I2C 地址（含读写位）。 */
    uint8_t es8311_8bit = 0;
    /** @brief I2S 播放通道句柄（i2s_chan_handle_t，audio_codec data_if 用）。 */
    void* tx_channel = nullptr;
    /** @brief I2S 采集通道句柄（i2s_chan_handle_t）。 */
    void* rx_channel = nullptr;
    /** @brief 采样率（官方 16kHz）。 */
    int sample_rate_hz = 16000;
};

/**
 * @brief 初始化 ES8311 Codec 控制面（官方小智方式：esp_codec_dev 组件）。
 *
 * 建立 I2C 控制接口 + I2S 数据接口 + ES8311 codec，打开 IN_OUT 设备
 * （16-bit / 1ch / sample_rate / MCLK 由 I2S 提供，官方 x256）。PA 功放
 * 不在此接管（pa_pin=-1），统一由 GPIO46 板级仲裁处理。初始化后读回
 * 关键寄存器（REG00/01/09/0A/17）并打印，供实板验证时钟锁定。
 * host 构建不触碰硬件，返回 kUnavailable。
 * @param config 初始化参数。
 * @return 初始化结果；成功时 value 为 esp_codec_dev 句柄（归属调用方，
 * 由 AudioPorts 持有并在 Close 时释放）。
 */
[[nodiscard]] voicelife::Result<void*> InitializeEs8311(const Es8311ControlConfig& config);

/**
 * @brief 释放 ES8311 Codec 设备（关闭 + 删除）。
 * @param dev_handle 由 InitializeEs8311 返回的句柄。
 * @return 释放结果。
 */
[[nodiscard]] voicelife::Status DeinitializeEs8311(void* dev_handle);

}  // namespace voicelife::audio_esp
