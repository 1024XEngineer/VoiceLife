#include "esp_log.h"
#include "platform_assemblies.h"
#include "voicelife/runtime/runtime.h"

namespace {

constexpr char kTag[] = "VoiceLife";

}  // namespace

extern "C" void app_main() {
    // 构建期选定板型装配（Profile -> PlatformAssembly）；Runtime 只依赖
    // PlatformAssembly 接口，不判断板型。static 局部保证 Assembly 生命周期
    // 覆盖整个程序运行期（Runtime 持引用，不能使用栈局部）。
#ifdef CONFIG_VOICELIFE_BOARD_ESP_SPARKBOT
    static voicelife::runtime::SparkBotAssembly assembly;
#else
    static voicelife::runtime::VoiceLifePcbAssembly assembly;
#endif
    const voicelife::Status status = voicelife::runtime::Start(assembly);
    if (!status.ok()) {
        ESP_LOGE(kTag, "启动失败：%s", status.message.c_str());
        return;
    }
    ESP_LOGI(kTag, "VoiceLife 架构主干已启动");
}
