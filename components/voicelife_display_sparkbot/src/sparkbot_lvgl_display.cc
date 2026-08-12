#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"

namespace voicelife::display_sparkbot {

SparkBotLvglDisplay::SparkBotLvglDisplay(const SparkBotLcdConfig& config) : config_(config) {}

SparkBotLvglDisplay::~SparkBotLvglDisplay() = default;

voicelife::Status SparkBotLvglDisplay::Initialize() {
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable,
                                    "SparkBot ST7789/LVGL 初始化尚未移植；官方 Renderer 移植阶段按 "
                                    "xiaozhi-esp32 官方实现接入（SpiLcdDisplay + esp_sparkbot_board）");
}

void* SparkBotLvglDisplay::display_handle() { return nullptr; }

}  // namespace voicelife::display_sparkbot
