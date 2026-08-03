#pragma once

#include <atomic>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voicelife/contracts/status.h"

namespace voicelife::timing { class TimingTaskRunner; }

namespace voicelife::timing_freertos {

class TimingTaskLoop final {
   public:
    explicit TimingTaskLoop(timing::TimingTaskRunner& runner) : runner_(runner) {}
    ~TimingTaskLoop() { Stop(); }
    Status Start(uint32_t poll_interval_ms = 1000, uint32_t stack_size = 4096, unsigned priority = 5);
    void Stop();

   private:
    static void Entry(void* context);
    timing::TimingTaskRunner& runner_;
    std::atomic<TaskHandle_t> task_handle_{nullptr};
    uint32_t poll_interval_ms_ = 1000;
    std::atomic<bool> running_{false};
};

}  // namespace voicelife::timing_freertos
