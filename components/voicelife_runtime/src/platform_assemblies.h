#pragma once

#include "voicelife/display_esp/sparkbot_presentation_adapter.h"
#include "voicelife/display_esp/ssd1306_presentation_adapter.h"
#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"
#include "voicelife/runtime/platform_assembly.h"

namespace voicelife::runtime {

/**
 * @brief VoiceLife PCB 平台装配：Ssd1306PresentationAdapter。
 *
 * 旧 VoiceLife PCB 继续走点阵 SSD1306 显示路径；本包装是迁移起点，
 * 不是冻结旧板的永久实现，后续可独立演进。
 */
class VoiceLifePcbAssembly : public PlatformAssembly {
   public:
    /** @brief 虚析构函数。 */
    ~VoiceLifePcbAssembly() override = default;

    /** @brief 返回点阵显示端口。 @return Ssd1306PresentationAdapter。 */
    voicelife::voice::PresentationPort& presentation() override;

   private:
    voicelife::display_esp::Ssd1306PresentationAdapter ssd1306_adapter_;
};

/**
 * @brief ESP-SparkBot 平台装配：SparkBotPresentationAdapter + LVGL 显示。
 *
 * 官方 Renderer 未移植前，presentation() 返回的端口 available=false，
 * Render/Submit 明确返回 kUnavailable，不伪装已支持；Start() 驱动
 * ST7789/LVGL 初始化（官方移植），host 构建返回 kUnavailable。
 */
class SparkBotAssembly : public PlatformAssembly {
   public:
    /** @brief 构造函数：按官方板级 Profile 准备 LVGL 显示配置。 */
    SparkBotAssembly();

    /** @brief 虚析构函数。 */
    ~SparkBotAssembly() override = default;

    /** @brief 返回 SparkBot 彩屏显示端口。 @return SparkBotPresentationAdapter。 */
    voicelife::voice::PresentationPort& presentation() override;

    /** @brief 初始化 ST7789/LVGL（官方移植）。 @return 初始化结果。 */
    voicelife::Status Start() override;

   private:
    voicelife::display_esp::SparkBotPresentationAdapter sparkbot_adapter_;
    voicelife::display_sparkbot::SparkBotLvglDisplay lvgl_display_;
};

}  // namespace voicelife::runtime
