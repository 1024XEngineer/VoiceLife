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
constexpr voicelife::voice::DisplayCapabilities kSparkBotCapabilities{
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

/** @brief 官方牛头表情的受控标识列表（与 manifest.json 一致）。 */
constexpr std::array<std::string_view, 10> kControlledAssetIds = {
    "boot", "connecting", "error", "happy", "idle", "listening", "provisioning", "sleepy", "speaking", "thinking",
};

bool IsPathLike(std::string_view asset_id) {
    return asset_id.empty() || asset_id.find('/') != std::string_view::npos ||
           asset_id.find('\\') != std::string_view::npos || asset_id.find("..") != std::string_view::npos;
}

#ifdef ESP_PLATFORM
constexpr const char* kTag = "sparkbot_adapter";
constexpr uint32_t kDisplayTaskStackWords = 4096;
constexpr uint32_t kDisplayTaskPriority = 1;
#endif

}  // namespace

SparkBotPresentationAdapter::SparkBotPresentationAdapter(const SparkBotLcdConfig& config)
    : display_(config), queue_(kQueueCapacity) {}

SparkBotPresentationAdapter::~SparkBotPresentationAdapter() { (void)Stop(); }

const voicelife::voice::DisplayCapabilities& SparkBotPresentationAdapter::capabilities() const {
    return kSparkBotCapabilities;
}

voicelife::Status SparkBotPresentationAdapter::Render(const voicelife::voice::DisplaySnapshot& snapshot) {
    queue_.Push(snapshot);
    return voicelife::Status::Ok();
}

voicelife::Status SparkBotPresentationAdapter::Submit(voicelife::voice::PresentationCommand command) {
    if (IsPathLike(command.asset_id)) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInvalidArgument,
                                        "资源标识必须是非空、无路径分隔符的受控名称");
    }
    const bool controlled = std::find(kControlledAssetIds.begin(), kControlledAssetIds.end(), command.asset_id) !=
                            kControlledAssetIds.end();
    if (!controlled) {
        return voicelife::Status::Error(voicelife::ErrorCode::kNotFound, "资源不在官方 SparkBot 资源清单中");
    }
    // 独立资源命令（如预览图）本阶段未实现；动画由 Render 快照 mood 驱动。
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "独立资源命令未实现；动画由 Render 快照驱动");
}

voicelife::Status SparkBotPresentationAdapter::Start() {
#ifdef ESP_PLATFORM
    if (started_) {
        return voicelife::Status::Ok();
    }
    const voicelife::Status init = display_.Initialize();
    if (!init.ok()) {
        return init;
    }
    if (xTaskCreate(DisplayTaskEntry, "sparkbot_disp", kDisplayTaskStackWords, this, kDisplayTaskPriority,
                    static_cast<TaskHandle_t*>(&task_handle_)) != pdPASS) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "显示任务创建失败");
    }
    started_ = true;
    return voicelife::Status::Ok();
#else
    (void)0;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不启动真实显示任务");
#endif
}

voicelife::Status SparkBotPresentationAdapter::Stop() {
#ifdef ESP_PLATFORM
    if (task_handle_ != nullptr) {
        vTaskDelete(static_cast<TaskHandle_t>(task_handle_));
        task_handle_ = nullptr;
    }
    started_ = false;
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
    while (queue_.WaitPop(&snapshot, 0)) {
        if (snapshot.revision <= last_rendered_revision_) {
            continue;  // 旧 revision 丢弃：防止旧状态覆盖新状态。
        }
        last_rendered_revision_ = snapshot.revision;
        if (lvgl_port_lock(0) == ESP_OK) {
            (void)renderer_.Render(snapshot);
            lvgl_port_unlock();
        }
    }
    // 队列因 Stop 唤醒后任务退出。
#endif
}

}  // namespace voicelife::display_sparkbot
