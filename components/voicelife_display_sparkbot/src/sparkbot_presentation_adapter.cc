#include "voicelife/display_sparkbot/sparkbot_presentation_adapter.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "voicelife/display_sparkbot/sparkbot_emoji_assets.h"

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace voicelife::display_sparkbot {

namespace {

constexpr std::size_t kQueueCapacity = 8;

/** @brief SparkBot 彩屏能力声明：显示链路已闭合（代码级）。 */
[[maybe_unused]] constexpr voicelife::voice::DisplayCapabilities kSparkBotCapabilities{
    .available = true,
    .text = true,
    .static_image = true,
    .animation = true,
    .preview_image = false,
    // 240x240 ST7789 RGB565 单帧缓冲硬件上限。
    .max_frame_bytes = 240U * 240U * 2U,
    // 官方 LVGL 刷新节奏尚未实板核实，保持 0（未确认）。
    .refresh_budget_hz = 0U,
};

}  // namespace

bool ShouldDropDisplaySnapshot(uint64_t generation, uint64_t revision, uint64_t last_generation,
                               uint64_t last_revision) {
    return generation < last_generation || (generation == last_generation && revision <= last_revision);
}

namespace {
#ifdef ESP_PLATFORM
constexpr const char* kTag = "sparkbot_adapter";
constexpr uint32_t kDisplayTaskStackWords = 4096;
constexpr uint32_t kDisplayTaskPriority = 1;
#endif
}  // namespace

SparkBotPresentationAdapter::SparkBotPresentationAdapter(const SparkBotLcdConfig& config, BacklightCallback backlight)
    : display_(config), queue_(kQueueCapacity), backlight_cb_(std::move(backlight)) {
    // 显示链路声明硬件能力，但 available 只在显示启动成功后置真；
    // 初始化失败时保持 false，不产生“运行正常但屏幕不可用”的假成功。
    capabilities_ = kSparkBotCapabilities;
    capabilities_.available = false;
}

SparkBotPresentationAdapter::~SparkBotPresentationAdapter() { (void)Stop(); }

const voicelife::voice::DisplayCapabilities& SparkBotPresentationAdapter::capabilities() const { return capabilities_; }

voicelife::Status SparkBotPresentationAdapter::Render(const voicelife::voice::DisplaySnapshot& snapshot) {
    queue_.Push(snapshot);
    UpdateBacklight(snapshot);
    return voicelife::Status::Ok();
}

void SparkBotPresentationAdapter::UpdateBacklight(const voicelife::voice::DisplaySnapshot& /*snapshot*/) {
    // 生产运行期待机保持背光（idle GIF 长期可见）；省电必须引入显式
    // DisplayPowerMode，不能由交互状态推导，因此这里不随 standby 关背光。
    if (!backlight_on_ && backlight_cb_) {
        backlight_cb_(true);
        backlight_on_ = true;
    }
}

voicelife::Status SparkBotPresentationAdapter::Start() {
#ifdef ESP_PLATFORM
    if (started_) {
        return voicelife::Status::Ok();
    }
    const voicelife::Status init = display_.Initialize();
    if (!init.ok()) {
        return init;  // 显示初始化失败：available 保持 false，调用方可诊断并停止。
    }
    capabilities_.available = true;
    if (xTaskCreate(DisplayTaskEntry, "sparkbot_disp", kDisplayTaskStackWords, this, kDisplayTaskPriority,
                    reinterpret_cast<TaskHandle_t*>(&task_handle_)) != pdPASS) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "显示任务创建失败");
    }
    started_ = true;
    ESP_LOGI(kTag, "SPARKBOT_DISPLAY_TASK_STARTED=1");
    return voicelife::Status::Ok();
#else
    (void)0;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不启动真实显示任务");
#endif
}

voicelife::Status SparkBotPresentationAdapter::Stop() {
#ifdef ESP_PLATFORM
    if (started_) {
        // 请求退出并等待任务自行 vTaskDelete（不能删除条件变量等待中的任务）。
        stop_requested_ = true;
        for (int i = 0; i < 50 && !task_exited_.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        task_handle_ = nullptr;
        started_ = false;
    }
    return voicelife::Status::Ok();
#else
    (void)0;
    return voicelife::Status::Ok();
#endif
}

void SparkBotPresentationAdapter::DisplayTaskEntry(void* arg) {
    auto* self = static_cast<SparkBotPresentationAdapter*>(arg);
    self->DisplayTaskLoop();
}

void SparkBotPresentationAdapter::DisplayTaskLoop() {
#ifdef ESP_PLATFORM
    // 显示任务只消费快照；所有 LVGL 调用都在 lvgl_port 锁内执行。
    voicelife::voice::DisplaySnapshot snapshot;
    while (!stop_requested_.load()) {
        if (!queue_.WaitPop(&snapshot, 50)) {
            continue;  // 超时轮询停止请求。
        }
        // 先比 generation（旧回合整体丢弃），再比 revision（同回合旧状态丢弃）。
        if (ShouldDropDisplaySnapshot(snapshot.generation, snapshot.revision, last_generation_,
                                      last_rendered_revision_)) {
            continue;  // 迟到快照：防止旧状态覆盖新状态。
        }
        // esp_lvgl_port returns bool; 0 means wait indefinitely, not success.
        // The dedicated display task may block here, but producers never do.
        if (lvgl_port_lock(0)) {
            const voicelife::Status render_status = renderer_.Render(snapshot);
            if (!render_status.ok()) {
                ESP_LOGW(kTag, "SPARKBOT_DISPLAY_RENDER_FAILED code=%d", static_cast<int>(render_status.code));
            } else {
                // 锁内推进基准：锁失败时不推进，快照将在下次重试。
                last_generation_ = snapshot.generation;
                last_rendered_revision_ = snapshot.revision;
                ESP_LOGI(kTag, "SPARKBOT_DISPLAY_SNAPSHOT_RENDERED generation=%llu revision=%llu",
                         static_cast<unsigned long long>(snapshot.generation),
                         static_cast<unsigned long long>(snapshot.revision));
            }
            lvgl_port_unlock();
        }
    }
    task_exited_ = true;
    vTaskDelete(nullptr);  // 任务自行退出，避免删除等待中的任务。
#endif
}

}  // namespace voicelife::display_sparkbot
