#pragma once

#include <cstdint>

#include "voicelife/contracts/status.h"

namespace voicelife::display_sparkbot {

/** @brief ST7789/LVGL 初始化所需板级参数（调用方从 SparkBotProfile 提供）。 */
struct SparkBotLcdConfig {
    /** @brief SPI 控制器编号（官方 SPI3_HOST）。 */
    int spi_host = 3;
    /** @brief SPI mode（官方 SparkBot 为 mode 2，mode 0 无图像）。 */
    int spi_mode = 2;
    /** @brief 像素时钟，单位 Hz（官方 40MHz）。 */
    uint32_t pixel_clock_hz = 40U * 1000U * 1000U;
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
    /** @brief 显示宽度。 */
    int width = 240;
    /** @brief 显示高度。 */
    int height = 240;
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

/**
 * @brief SparkBot ST7789/LVGL 显示初始化与渲染入口（官方移植骨架）。
 *
 * 官方 Renderer 移植完成前不执行任何初始化或渲染：Initialize 返回
 * kUnavailable，不伪装已支持。移植唯一来源（不得自行重新设计 UI）：
 *   - xiaozhi-esp32@37d1aee main/display/lcd_display.cc（SpiLcdDisplay）
 *   - xiaozhi-esp32@37d1aee main/boards/espressif/esp-sparkbot/
 * LVGL 对象、GIF 解码、缓存、像素缓冲区和刷新任务只存在于本显示上下文；
 * Provider 回调、音频实时任务、输入源不得直接调用渲染。
 */
class SparkBotLvglDisplay {
   public:
    /** @brief 构造函数。 @param config 官方 SparkBot 板级显示参数。 */
    explicit SparkBotLvglDisplay(const SparkBotLcdConfig& config);
    /** @brief 虚析构函数。 */
    ~SparkBotLvglDisplay();

    SparkBotLvglDisplay(const SparkBotLvglDisplay&) = delete;
    SparkBotLvglDisplay& operator=(const SparkBotLvglDisplay&) = delete;

    /**
     * @brief 初始化 SPI/ST7789 面板与 LVGL（官方移植）。
     *
     * 骨架阶段返回 kUnavailable。
     * @return 初始化结果。
     */
    [[nodiscard]] voicelife::Status Initialize();

    /**
     * @brief 返回 LVGL display 句柄（lv_display_t*）。
     *
     * 公共头不暴露 LVGL 类型；返回 void*，仅在 Adapter 内部还原。
     * @return LVGL display 句柄，未初始化时为 nullptr。
     */
    void* display_handle();

   private:
    SparkBotLcdConfig config_;
};

}  // namespace voicelife::display_sparkbot
