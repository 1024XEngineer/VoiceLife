#include "platform_assemblies.h"

#include "voicelife/board_esp/sparkbot_profile.h"
#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"

#ifdef ESP_PLATFORM
#include <driver/gpio.h>
#endif

namespace voicelife::runtime {

namespace {

/** @brief 从官方 SparkBot 板级 Profile 填充 LVGL 显示配置。 */
voicelife::display_sparkbot::SparkBotLcdConfig MakeSparkBotLcdConfig() {
    const auto profile = voicelife::board_esp::SparkBotProfile();
    voicelife::display_sparkbot::SparkBotLcdConfig config;
    config.spi_host = profile.display.spi_host;
    config.spi_mode = profile.display.spi_mode;
    config.pixel_clock_hz = profile.display.pixel_clock_hz;
    config.dc_gpio = profile.display.dc_gpio;
    config.cs_gpio = profile.display.cs_gpio;
    config.clk_gpio = profile.display.clk_gpio;
    config.mosi_gpio = profile.display.mosi_gpio;
    config.reset_gpio = profile.display.reset_gpio;
    config.width = profile.display.width;
    config.height = profile.display.height;
    config.offset_x = profile.display.offset_x;
    config.offset_y = profile.display.offset_y;
    config.mirror_x = profile.display.mirror_x;
    config.mirror_y = profile.display.mirror_y;
    config.swap_xy = profile.display.swap_xy;
    return config;
}

}  // namespace

voicelife::voice::PresentationPort& VoiceLifePcbAssembly::presentation() { return ssd1306_adapter_; }

SparkBotAssembly::SparkBotAssembly()
    : arbiter_(voicelife::board_esp::SparkBotProfile().shared_power),
      adapter_(MakeSparkBotLcdConfig(), [this](bool enabled) { ApplyBacklight(enabled); }) {}

voicelife::voice::PresentationPort& SparkBotAssembly::presentation() { return adapter_; }

voicelife::Status SparkBotAssembly::Start() {
    // 显示启动：经统一仲裁启用背光。
    ApplyBacklight(true);
    return adapter_.Start();
}

void SparkBotAssembly::ApplyBacklight(bool enabled) {
    (void)arbiter_.SetBacklightEnabled(enabled);
#ifdef ESP_PLATFORM
    const auto profile = voicelife::board_esp::SparkBotProfile().shared_power;
    if (profile.gpio >= 0) {
        const int level = arbiter_.line_enabled() ? (profile.active_high ? 1 : 0) : (profile.active_high ? 0 : 1);
        (void)gpio_set_level(static_cast<gpio_num_t>(profile.gpio), level);
    }
#else
    (void)enabled;
#endif
}

}  // namespace voicelife::runtime
