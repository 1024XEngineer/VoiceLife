#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "voicelife/contracts/status.h"

namespace voicelife::board_esp {

/** @brief SparkBot 能力证据状态。 */
enum class CapabilityStatus : uint8_t {
    /** @brief 已有可靠硬件证据。 */
    kVerified,
    /** @brief 已有可靠证据证明能力不存在。 */
    kAbsent,
    /** @brief 尚需原理图、BOM 或实板测试。 */
    kNeedsBoardTest,
};

/** @brief 板级能力标识。 */
enum class BoardCapability : uint8_t {
    /** @brief ESP32-S3 芯片。 */
    kChip,
    /** @brief Flash 容量。 */
    kFlash,
    /** @brief PSRAM 容量。 */
    kPsram,
    /** @brief ST7789 彩屏。 */
    kDisplay,
    /** @brief ES8311 双工 Codec。 */
    kAudioCodec,
    /** @brief OV2640 摄像头。 */
    kCamera,
    /** @brief 底盘 UART。 */
    kChassis,
    /** @brief BOOT 按键。 */
    kBootButton,
    /** @brief GPIO46 功放/背光共享电源线。 */
    kSharedPower,
    /** @brief IMU。 */
    kImu,
    /** @brief ToF。 */
    kTof,
    /** @brief 环境光传感器。 */
    kAmbientLight,
    /** @brief 触摸输入。 */
    kTouch,
};

/** @brief 能力状态及其证据来源。 */
struct CapabilityEvidence {
    /** @brief 能力标识。 */
    BoardCapability capability = BoardCapability::kChip;
    /** @brief 当前证据状态。 */
    CapabilityStatus status = CapabilityStatus::kNeedsBoardTest;
    /** @brief 不包含凭据的证据说明。 */
    std::string evidence;
};

/** @brief ST7789 的官方 SparkBot 总线与几何参数。 */
struct SparkBotDisplayProfile {
    /** @brief SPI 控制器编号，3 对应 SPI3_HOST。 */
    uint8_t spi_host = 3;
    /** @brief SPI mode。 */
    uint8_t spi_mode = 2;
    /** @brief 像素时钟，单位 Hz。 */
    uint32_t pixel_clock_hz = 40U * 1000U * 1000U;
    /** @brief 显示宽度。 */
    uint16_t width = 240;
    /** @brief 显示高度。 */
    uint16_t height = 240;
    /** @brief DC GPIO。 */
    int dc_gpio = -1;
    /** @brief CS GPIO。 */
    int cs_gpio = -1;
    /** @brief SPI CLK GPIO。 */
    int clk_gpio = -1;
    /** @brief SPI MOSI GPIO。 */
    int mosi_gpio = -1;
    /** @brief 复位 GPIO，-1 表示未连接。 */
    int reset_gpio = -1;
    /** @brief 背光 GPIO。 */
    int backlight_gpio = -1;
    /** @brief X 方向偏移。 */
    int offset_x = 0;
    /** @brief Y 方向偏移。 */
    int offset_y = 0;
    /** @brief 是否镜像 X。 */
    bool mirror_x = false;
    /** @brief 是否镜像 Y。 */
    bool mirror_y = false;
    /** @brief 是否交换 XY。 */
    bool swap_xy = false;
};

/** @brief ES8311 双工音频的板级引脚与总线参数。 */
struct SparkBotAudioProfile {
    /** @brief Codec 控制 I2C 端口。 */
    uint8_t i2c_port = 0;
    /** @brief I2C SDA GPIO。 */
    int i2c_sda_gpio = -1;
    /** @brief I2C SCL GPIO。 */
    int i2c_scl_gpio = -1;
    /** @brief ES8311 的 7-bit I2C 地址。 */
    uint8_t es8311_i2c_address_7bit = 0;
    /** @brief 采集采样率，单位 Hz。 */
    uint32_t input_sample_rate_hz = 16000;
    /** @brief 播放采样率，单位 Hz。 */
    uint32_t output_sample_rate_hz = 16000;
    /** @brief MCLK GPIO。 */
    int mclk_gpio = -1;
    /** @brief WS GPIO。 */
    int ws_gpio = -1;
    /** @brief BCLK GPIO。 */
    int bclk_gpio = -1;
    /** @brief Codec 采集数据 GPIO。 */
    int din_gpio = -1;
    /** @brief Codec 播放数据 GPIO。 */
    int dout_gpio = -1;
};

/** @brief OV2640 DVP 摄像头的官方 SparkBot 引脚参数。 */
struct SparkBotCameraProfile {
    /** @brief 8-bit DVP 数据 GPIO，按 D0 到 D7 排列。 */
    std::array<int, 8> data_gpio{};
    /** @brief XCLK GPIO。 */
    int xclk_gpio = -1;
    /** @brief PCLK GPIO。 */
    int pclk_gpio = -1;
    /** @brief VSYNC GPIO。 */
    int vsync_gpio = -1;
    /** @brief HSYNC/DE GPIO。 */
    int hsync_gpio = -1;
    /** @brief XCLK 频率，单位 Hz。 */
    uint32_t xclk_frequency_hz = 16U * 1000U * 1000U;
    /** @brief SCCB SDA GPIO，-1 表示复用已有 I2C。 */
    int sccb_sda_gpio = -1;
    /** @brief SCCB SCL GPIO，-1 表示复用已有 I2C。 */
    int sccb_scl_gpio = -1;
    /** @brief PWDN GPIO，-1 表示未连接。 */
    int pwdn_gpio = -1;
    /** @brief RESET GPIO，-1 表示未连接。 */
    int reset_gpio = -1;
};

/** @brief 底盘 UART 的板级参数。 */
struct SparkBotChassisProfile {
    /** @brief UART 控制器编号。 */
    uint8_t uart_port = 1;
    /** @brief UART 波特率。 */
    uint32_t baud_rate = 115200;
    /** @brief TX GPIO。 */
    int tx_gpio = -1;
    /** @brief RX GPIO。 */
    int rx_gpio = -1;
};

/** @brief GPIO46 功放和背光共享线的仲裁输入。 */
struct SharedPowerProfile {
    /** @brief 共享 GPIO。 */
    int gpio = -1;
    /** @brief 高电平是否代表启用。 */
    bool active_high = true;
    /** @brief 背光是否连接到共享线。 */
    bool backlight_shared = false;
    /** @brief 音频输出是否连接到共享线。 */
    bool audio_output_shared = false;
};

/** @brief SparkBot 完整板级事实和能力证据。 */
struct SparkBotBoardProfile {
    /** @brief Profile ID。 */
    std::string id;
    /** @brief 官方 SKU。 */
    std::string sku;
    /** @brief ESP-IDF 构建目标。 */
    std::string target;
    /** @brief 预期 Flash 容量，单位字节。 */
    uint32_t expected_flash_bytes = 0;
    /** @brief 预期 PSRAM 容量，单位字节。 */
    uint32_t expected_psram_bytes = 0;
    /** @brief 显示板级参数。 */
    SparkBotDisplayProfile display;
    /** @brief 音频板级参数。 */
    SparkBotAudioProfile audio;
    /** @brief 摄像头板级参数。 */
    SparkBotCameraProfile camera;
    /** @brief 底盘串口参数。 */
    SparkBotChassisProfile chassis;
    /** @brief BOOT GPIO。 */
    int boot_button_gpio = -1;
    /** @brief 共享电源参数。 */
    SharedPowerProfile shared_power;
    /** @brief 能力矩阵。 */
    std::array<CapabilityEvidence, 13> capabilities{};

    /** @brief 校验板级事实之间没有明显冲突。 @return 合法返回 Ok。 */
    [[nodiscard]] Status Validate() const;
};

/**
 * @brief 返回官方 SparkBot 板级 Profile。
 * @return 按官方板级配置填充的 SparkBot Profile。
 */
[[nodiscard]] SparkBotBoardProfile SparkBotProfile();

/**
 * @brief 查找能力矩阵中的指定能力。
 * @param profile 要查询的 SparkBot Profile。
 * @param capability 要查询的能力。
 * @return 找到的能力证据；非法枚举值返回空指针。
 */
[[nodiscard]] const CapabilityEvidence* FindCapability(const SparkBotBoardProfile& profile, BoardCapability capability);

}  // namespace voicelife::board_esp
