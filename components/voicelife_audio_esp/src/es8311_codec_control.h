#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::audio_esp {

/**
 * @brief ES8311 Codec I2C 初始化（官方小智 SparkBot 寄存器流程移植）。
 *
 * 只负责 ES8311 寄存器写入（软复位 -> 初始化序列 -> 16kHz 采样率 ->
 * I2S Philips 16bit -> 启动序列），不持有 I2S 通道。MCLK 由 I2S 输出
 * （12.288MHz = 16kHz * 768）。实板验证阶段按 ACK/录放音证据校正。
 * host 构建不触碰 I2C，返回 kUnavailable。
 *
 * @param i2c_port I2C 控制器编号。
 * @param sda_gpio SDA GPIO。
 * @param scl_gpio SCL GPIO。
 * @param es8311_8bit ES8311 8-bit I2C 地址（含读写位）。
 * @return 初始化结果。
 */
[[nodiscard]] voicelife::Status InitializeEs8311(int i2c_port, int sda_gpio, int scl_gpio, uint8_t es8311_8bit);

}  // namespace voicelife::audio_esp
