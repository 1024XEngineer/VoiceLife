#include "voicelife/timing_esp/esp_timing_runtime.h"

#include <utility>

#ifdef ESP_PLATFORM

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <new>
#include <type_traits>
#include <variant>

namespace voicelife::timing_esp {

using namespace timing;

namespace {

constexpr UBaseType_t kCommandQueueDepth = 32;
// ESP-IDF defines FreeRTOS task stack depths in bytes, unlike upstream FreeRTOS.
constexpr configSTACK_DEPTH_TYPE kRunnerStackBytes = 8192;
constexpr UBaseType_t kRunnerPriority = 4;
constexpr char kTag[] = "VoiceLifeTiming";

class EspSystemClock final : public ClockPort {
   public:
    TriggerAt Now() override {
        return std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
    }
};

class EspOneShotTimer final : public OneShotTimerPort {
   public:
    ~EspOneShotTimer() override {
        if (timer_ != nullptr) {
            ClearExpiryCallbackAndWait();
            (void)esp_timer_delete(timer_);
        }
        if (quiesced_ != nullptr) {
            vSemaphoreDelete(quiesced_);
        }
    }

    bool Initialize() {
        quiesced_ = xSemaphoreCreateBinary();
        if (quiesced_ == nullptr) {
            return false;
        }
        esp_timer_create_args_t args{};
        args.callback = &TimerEntry;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "voicelife_timing";
        return esp_timer_create(&args, &timer_) == ESP_OK;
    }

    void SetExpiryCallback(ExpiryCallback callback) override { callback_ = std::move(callback); }

    void ClearExpiryCallbackAndWait() override {
        if (timer_ == nullptr || quiesced_callback_) {
            callback_ = {};
            return;
        }
        quiescing_.store(true);
        const auto stop_result = esp_timer_stop(timer_);
        if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
            std::abort();
        }
        while (xSemaphoreTake(quiesced_, 0) == pdTRUE) {
        }
        portENTER_CRITICAL(&barrier_lock_);
        barrier_armed_ = true;
        const auto barrier_result = esp_timer_start_once(timer_, 1);
        if (barrier_result != ESP_OK) {
            barrier_armed_ = false;
        }
        portEXIT_CRITICAL(&barrier_lock_);
        if (barrier_result != ESP_OK || xSemaphoreTake(quiesced_, portMAX_DELAY) != pdTRUE) {
            std::abort();
        }
        while (callback_accessing_owner_.load()) {
            taskYIELD();
        }
        callback_ = {};
        quiesced_callback_ = true;
    }

    Status ArmAfter(std::chrono::microseconds delay) override {
        const auto stop_result = esp_timer_stop(timer_);
        if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
            return Status::Error(ErrorCode::kInternal, "停止已有 ESP Timing timer 失败");
        }
        const auto delay_us = delay.count() > 0 ? static_cast<uint64_t>(delay.count()) : 1ULL;
        const auto result = esp_timer_start_once(timer_, delay_us);
        if (result == ESP_OK) {
            return Status::Ok();
        }
        return Status::Error(result == ESP_ERR_NO_MEM ? ErrorCode::kUnavailable : ErrorCode::kInternal,
                             "设置 ESP Timing timer 失败");
    }

    Status Disarm() override {
        const auto result = esp_timer_stop(timer_);
        if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
            return Status::Ok();
        }
        return Status::Error(ErrorCode::kInternal, "停止 ESP Timing timer 失败");
    }

   private:
    static void TimerEntry(void* argument) {
        auto& timer = *static_cast<EspOneShotTimer*>(argument);
        timer.callback_accessing_owner_.store(true);
        if (timer.quiescing_.load()) {
            bool completes_barrier = false;
            portENTER_CRITICAL(&timer.barrier_lock_);
            if (timer.barrier_armed_) {
                timer.barrier_armed_ = false;
                (void)esp_timer_stop(timer.timer_);
                completes_barrier = true;
            }
            portEXIT_CRITICAL(&timer.barrier_lock_);
            if (completes_barrier) {
                xSemaphoreGive(timer.quiesced_);
            }
            timer.callback_accessing_owner_.store(false);
            return;
        }
        if (timer.callback_) {
            timer.callback_();
        }
        timer.callback_accessing_owner_.store(false);
    }

    esp_timer_handle_t timer_ = nullptr;
    SemaphoreHandle_t quiesced_ = nullptr;
    ExpiryCallback callback_;
    std::atomic<bool> quiescing_{false};
    std::atomic<bool> callback_accessing_owner_{false};
    portMUX_TYPE barrier_lock_ = portMUX_INITIALIZER_UNLOCKED;
    bool barrier_armed_ = false;
    bool quiesced_callback_ = false;
};

class FreeRtosRunnerWake final : public RunnerWakePort {
   public:
    void SetRunnerTask(TaskHandle_t runner_task) { runner_task_ = runner_task; }

    void Notify() override {
        configASSERT(runner_task_ != nullptr);
        const auto result = xTaskNotifyGive(runner_task_);
        configASSERT(result == pdPASS);
        (void)result;
    }

   private:
    TaskHandle_t runner_task_ = nullptr;
};

using QueuedCommand = std::variant<RegisterTaskCommand, CancelTaskCommand>;

}  // namespace

class EspTimingTaskRuntime::Impl {
   public:
    Impl() : runtime_(runner_, clock_, timer_, wake_) {}

    ~Impl() { Stop(); }

    bool Start() {
        command_queue_ = xQueueCreate(kCommandQueueDepth, sizeof(QueuedCommand*));
        stopped_ = xSemaphoreCreateBinary();
        submission_mutex_ = xSemaphoreCreateMutex();
        if (command_queue_ == nullptr || stopped_ == nullptr || submission_mutex_ == nullptr || !timer_.Initialize()) {
            return false;
        }
        if (xTaskCreate(&RunnerEntry, "voicelife_timing", kRunnerStackBytes, this, kRunnerPriority, &runner_task_) !=
            pdPASS) {
            runner_task_ = nullptr;
            return false;
        }
        wake_.SetRunnerTask(runner_task_);
        accepting_.store(true);
        return true;
    }

    CommandAcceptance RegisterTask(RegisterTaskCommand command) {
        if (runner_.IsInCallbackContext()) {
            return runner_.RegisterTask(std::move(command));
        }
        return Enqueue(std::move(command));
    }

    CommandAcceptance CancelTask(CancelTaskCommand command) {
        if (runner_.IsInCallbackContext()) {
            return runner_.CancelTask(std::move(command));
        }
        return Enqueue(std::move(command));
    }

   private:
    static void RunnerEntry(void* argument) {
        auto& self = *static_cast<Impl*>(argument);
        self.Run();
        vTaskDelete(nullptr);
    }

    template <typename Command>
    CommandAcceptance Enqueue(Command command) {
        if (submission_mutex_ == nullptr || xSemaphoreTake(submission_mutex_, 0) != pdTRUE) {
            return CommandAcceptance::kUnavailable;
        }
        struct SubmissionGuard {
            SemaphoreHandle_t mutex;
            ~SubmissionGuard() { xSemaphoreGive(mutex); }
        } submission_guard{submission_mutex_};
        if (!accepting_.load()) {
            return CommandAcceptance::kUnavailable;
        }
        auto* queued_command = new (std::nothrow) QueuedCommand(std::move(command));
        if (queued_command == nullptr) {
            return CommandAcceptance::kUnavailable;
        }
        if (xQueueSend(command_queue_, &queued_command, 0) != pdPASS) {
            delete queued_command;
            return CommandAcceptance::kUnavailable;
        }
        wake_.Notify();
        return CommandAcceptance::kAccepted;
    }

    void Run() {
        while (true) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (stopping_.load()) {
                DrainCommands();
                while (runner_.ProcessPendingCommands(clock_.Now()) != 0) {
                }
                timer_.ClearExpiryCallbackAndWait();
                break;
            }
            DrainCommands();
            while (!stopping_.load()) {
                const auto status = runtime_.ProcessWake();
                if (status.ok()) {
                    break;
                }
                ESP_LOGE(kTag, "TIMING_TIMER_ERROR code=%d msg=%s", static_cast<int>(status.code),
                         status.message.c_str());
                if (status.code == ErrorCode::kInternal) {
                    std::abort();
                }
                vTaskDelay(1);
                DrainCommands();
            }
        }
        xSemaphoreGive(stopped_);
    }

    void DrainCommands() {
        if (command_queue_ == nullptr) {
            return;
        }
        QueuedCommand* command = nullptr;
        while (xQueueReceive(command_queue_, &command, 0) == pdPASS) {
            std::visit(
                [this](auto&& value) {
                    using Command = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Command, RegisterTaskCommand>) {
                        runner_.RegisterTask(std::move(value));
                    } else {
                        runner_.CancelTask(std::move(value));
                    }
                },
                std::move(*command));
            delete command;
        }
    }

    void Stop() {
        if (runner_task_ != nullptr) {
            (void)xSemaphoreTake(submission_mutex_, portMAX_DELAY);
            accepting_.store(false);
            xSemaphoreGive(submission_mutex_);
            stopping_.store(true);
            wake_.Notify();
            (void)xSemaphoreTake(stopped_, portMAX_DELAY);
            runner_task_ = nullptr;
            wake_.SetRunnerTask(nullptr);
        }
        DrainCommands();
        if (command_queue_ != nullptr) {
            vQueueDelete(command_queue_);
            command_queue_ = nullptr;
        }
        if (stopped_ != nullptr) {
            vSemaphoreDelete(stopped_);
            stopped_ = nullptr;
        }
        if (submission_mutex_ != nullptr) {
            vSemaphoreDelete(submission_mutex_);
            submission_mutex_ = nullptr;
        }
    }

    InMemoryTimingTaskRunner runner_;
    EspSystemClock clock_;
    EspOneShotTimer timer_;
    FreeRtosRunnerWake wake_;
    TimingTaskRuntime runtime_;
    QueueHandle_t command_queue_ = nullptr;
    SemaphoreHandle_t stopped_ = nullptr;
    SemaphoreHandle_t submission_mutex_ = nullptr;
    TaskHandle_t runner_task_ = nullptr;
    std::atomic<bool> accepting_{false};
    std::atomic<bool> stopping_{false};
};

Result<std::unique_ptr<EspTimingTaskRuntime>> EspTimingTaskRuntime::Create() {
    auto impl = std::make_unique<Impl>();
    if (!impl->Start()) {
        return Result<std::unique_ptr<EspTimingTaskRuntime>>::Failure(ErrorCode::kUnavailable,
                                                                      "ESP Timing 平台资源创建失败");
    }
    return Result<std::unique_ptr<EspTimingTaskRuntime>>::Success(
        std::unique_ptr<EspTimingTaskRuntime>(new EspTimingTaskRuntime(std::move(impl))));
}

EspTimingTaskRuntime::EspTimingTaskRuntime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EspTimingTaskRuntime::~EspTimingTaskRuntime() = default;

CommandAcceptance EspTimingTaskRuntime::RegisterTask(RegisterTaskCommand command) {
    return impl_->RegisterTask(std::move(command));
}

CommandAcceptance EspTimingTaskRuntime::CancelTask(CancelTaskCommand command) {
    return impl_->CancelTask(std::move(command));
}

}  // namespace voicelife::timing_esp

#else

namespace voicelife::timing_esp {

class EspTimingTaskRuntime::Impl {};

Result<std::unique_ptr<EspTimingTaskRuntime>> EspTimingTaskRuntime::Create() {
    return Result<std::unique_ptr<EspTimingTaskRuntime>>::Failure(ErrorCode::kUnavailable,
                                                                  "ESP Timing 仅在 ESP 平台可用");
}

EspTimingTaskRuntime::EspTimingTaskRuntime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EspTimingTaskRuntime::~EspTimingTaskRuntime() = default;

CommandAcceptance EspTimingTaskRuntime::RegisterTask(RegisterTaskCommand) { return CommandAcceptance::kUnavailable; }

CommandAcceptance EspTimingTaskRuntime::CancelTask(CancelTaskCommand) { return CommandAcceptance::kUnavailable; }

}  // namespace voicelife::timing_esp

#endif
