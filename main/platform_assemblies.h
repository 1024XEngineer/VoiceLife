#pragma once

#include "voicelife/board_esp/gpio46_power_arbiter.h"
#include "voicelife/display_esp/ssd1306_presentation_adapter.h"
#include "voicelife/display_sparkbot/sparkbot_presentation_adapter.h"
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

    /** @brief 返回 VoiceLife PCB 音频 Profile。 @return PCM direct I2S simplex Profile。 */
    audio_esp::AudioBoardProfile audio_profile() const override;

    /** @brief 返回 VoiceLife PCB 按键 GPIO（boot/touch/volume_up/volume_down）。 @return 按键列表。 */
    std::vector<int> button_gpios() const override { return {0, 47, 40, 39}; }

   private:
    voicelife::display_esp::Ssd1306PresentationAdapter ssd1306_adapter_;
};

/**
 * @brief ESP-SparkBot 平台装配：完整 SparkBotPresentationAdapter。
 *
 * 调用链：presentation() -> 有界队列 -> 专属显示任务 -> 官方 Renderer；
 * Start() 初始化 ST7789/LVGL 并启动显示任务。实板显示未验证，available
 * 为 true 仅表示显示链路（代码级）闭合。
 */
class SparkBotAssembly : public PlatformAssembly {
   public:
    /** @brief 构造函数：按官方板级 Profile 准备显示配置。 */
    SparkBotAssembly();

    /** @brief 虚析构函数。 */
    ~SparkBotAssembly() override = default;

    /** @brief 返回 SparkBot 彩屏显示端口。 @return SparkBotPresentationAdapter。 */
    voicelife::voice::PresentationPort& presentation() override;

    /** @brief 初始化 ST7789/LVGL 并启动专属显示任务。 @return 启动结果。 */
    voicelife::Status Start() override;

    /** @brief 返回 SparkBot ES8311 双工音频 Profile。 @return ES8311 duplex Profile。 */
    audio_esp::AudioBoardProfile audio_profile() const override;

    /** @brief 返回 SparkBot 按键 GPIO（仅 BOOT）。 @return 按键列表。 */
    std::vector<int> button_gpios() const override { return {0}; }

    /** @brief 音频功放请求（经统一仲裁）。 @param enabled 是否启用功放。 @return 仲裁结果。 */
    voicelife::Status SetAudioOutputEnabled(bool enabled) override;

   private:
    /** @brief 经板级仲裁更新 GPIO46 背光（ESP 构建写 GPIO）。 */
    void ApplyBacklight(bool enabled);

    /** @brief 配置 GPIO46 为输出（唯一物理 owner，ESP 构建）。 */
    void ConfigureSharedPowerGpio();

    /** @brief 按仲裁结果写 GPIO46 电平（ESP 构建）。 */
    void WriteSharedPowerLine();

    voicelife::board_esp::Gpio46PowerArbiter arbiter_;
    voicelife::display_sparkbot::SparkBotPresentationAdapter adapter_;
};

}  // namespace voicelife::runtime
