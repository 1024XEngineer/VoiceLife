#include "esp_log.h"
#include "voicelife/runtime/runtime.h"

namespace {

constexpr char kTag[] = "VoiceLife";

}  // namespace

extern "C" void app_main() {
    const voicelife::Status status = voicelife::runtime::Start();
    if (!status.ok()) {
        ESP_LOGE(kTag, "启动失败：%s", status.message.c_str());
        return;
    }
    ESP_LOGI(kTag, "VoiceLife 架构主干已启动");
}
