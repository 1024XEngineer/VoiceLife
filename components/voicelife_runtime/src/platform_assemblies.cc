#include "platform_assemblies.h"

#include "voicelife/board_esp/sparkbot_profile.h"
#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"

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

SparkBotAssembly::SparkBotAssembly() : lvgl_display_(MakeSparkBotLcdConfig()) {}

voicelife::voice::PresentationPort& SparkBotAssembly::presentation() { return sparkbot_adapter_; }

voicelife::Status SparkBotAssembly::Start() { return lvgl_display_.Initialize(); }

}  // namespace voicelife::runtime
