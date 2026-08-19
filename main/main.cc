#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform_assemblies.h"
#include "sdkconfig.h"
#include "voicelife/runtime/runtime.h"

namespace {

constexpr char kTag[] = "VoiceLife";

#ifdef CONFIG_NVS_ENCRYPTION
constexpr int kNvsEncryption = CONFIG_NVS_ENCRYPTION;
#else
constexpr int kNvsEncryption = 0;
#endif

#ifdef CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID
constexpr int kNvsHmacKeyId = CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID;
#else
constexpr int kNvsHmacKeyId = 0;
#endif

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(kTag, "STARTUP_CONFIG nvs_encryption=%d hmac_key_id=%d", kNvsEncryption, kNvsHmacKeyId);
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
        // ESP-IDF 的 main task 不允许从 app_main 返回。保留故障现场，避免
        // 栈保护触发重启而覆盖串口诊断；恢复路径由后续刷写或人工复位执行。
        for (;;) vTaskDelay(portMAX_DELAY);
    }
    ESP_LOGI(kTag, "VoiceLife 架构主干已启动");
}
