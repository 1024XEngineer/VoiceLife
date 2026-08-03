#include "voicelife/timing_freertos/timing_task_loop.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voicelife/timing/timing_task_runner.h"

namespace voicelife::timing_freertos {
namespace {
constexpr char kTag[] = "voicelife_timing";
}

Status TimingTaskLoop::Start(uint32_t poll_interval_ms, uint32_t stack_size, unsigned priority) {
    if (running_.load()) return Status::Ok();
    if (poll_interval_ms == 0 || stack_size == 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "定时轮询参数无效");
    }
    poll_interval_ms_ = poll_interval_ms;
    running_.store(true);
    TaskHandle_t handle = nullptr;
    const BaseType_t created = xTaskCreate(&TimingTaskLoop::Entry, "voicelife_timing", stack_size, this,
                                           priority, &handle);
    if (created != pdPASS) {
        running_.store(false);
        return Status::Error(ErrorCode::kUnavailable, "无法创建定时任务 FreeRTOS task");
    }
    task_handle_.store(handle);
    return Status::Ok();
}

void TimingTaskLoop::Stop() {
    running_.store(false);
    TaskHandle_t handle = task_handle_.load();
    if (handle == nullptr) return;
    xTaskNotifyGive(handle);
    constexpr uint32_t kStopTimeoutMs = 1000;
    const TickType_t started_at = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(kStopTimeoutMs);
    while (task_handle_.load() != nullptr &&
           static_cast<TickType_t>(xTaskGetTickCount() - started_at) < timeout_ticks) {
        vTaskDelay(1);
    }
    handle = task_handle_.exchange(nullptr);
    if (handle != nullptr) {
        ESP_LOGE(kTag, "timing loop did not stop within %lu ms; deleting task",
                 static_cast<unsigned long>(kStopTimeoutMs));
        vTaskDelete(handle);
    }
}

void TimingTaskLoop::Entry(void* context) {
    auto* self = static_cast<TimingTaskLoop*>(context);
    while (self->running_.load()) {
        const Status status = self->runner_.PollDue();
        if (!status.ok()) {
            ESP_LOGE(kTag, "timing poll failed: code=%d message=%s",
                     static_cast<int>(status.code), status.message.c_str());
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(self->poll_interval_ms_));
    }
    self->task_handle_.store(nullptr);
    vTaskDelete(nullptr);
}

}  // namespace voicelife::timing_freertos
