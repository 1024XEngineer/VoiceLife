#include "voicelife/timing_esp/esp_timing_runtime.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <unity.h>

#include <atomic>
#include <chrono>
#include <string>

using namespace std::chrono_literals;
using voicelife::timing::CommandAcceptance;
using voicelife::timing::RegisterTaskCommand;
using voicelife::timing::RegisterTaskResult;
using voicelife::timing::TaskId;
using voicelife::timing::TriggerAt;
using voicelife::timing_esp::EspTimingTaskRuntime;

namespace {

TaskId RequireTaskId(std::string value) {
    auto task_id = TaskId::Create(std::move(value));
    TEST_ASSERT_TRUE(task_id.has_value());
    return std::move(*task_id);
}

RegisterTaskCommand Registration(std::string task_id, TriggerAt trigger_at) {
    return {
        .task_id = RequireTaskId(std::move(task_id)),
        .trigger_at = trigger_at,
        .callback = [](const TaskId&, TriggerAt) {},
        .on_result = [](RegisterTaskResult) {},
    };
}

}  // namespace

TEST_CASE("ESP Timing runtime creates and shuts down", "[timing_esp]") {
    auto created = EspTimingTaskRuntime::Create();

    TEST_ASSERT_TRUE(created.ok());
    TEST_ASSERT_TRUE(created.value.has_value());
    created.value->reset();
}

TEST_CASE("ESP Timing queue rejects work beyond its bounded capacity", "[timing_esp]") {
    auto created = EspTimingTaskRuntime::Create();
    TEST_ASSERT_TRUE(created.ok());
    auto runtime = std::move(*created.value);
    const auto trigger_at =
        std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now()) + 1h;
    size_t accepted = 0;
    size_t unavailable = 0;

    const auto original_priority = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, configMAX_PRIORITIES - 1);
    for (size_t index = 0; index < 64; ++index) {
        const auto result = runtime->RegisterTask(Registration("queued-" + std::to_string(index), trigger_at));
        accepted += result == CommandAcceptance::kAccepted ? 1 : 0;
        unavailable += result == CommandAcceptance::kUnavailable ? 1 : 0;
    }
    vTaskPrioritySet(nullptr, original_priority);

    TEST_ASSERT_GREATER_THAN(0, accepted);
    TEST_ASSERT_GREATER_THAN(0, unavailable);
}

TEST_CASE("ESP timer wakes the Runner before business callback execution", "[timing_esp]") {
    auto created = EspTimingTaskRuntime::Create();
    TEST_ASSERT_TRUE(created.ok());
    auto runtime = std::move(*created.value);
    auto* callback_done = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(callback_done);
    std::atomic<bool> registration_applied{false};
    const auto trigger_at =
        std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now()) + 20ms;

    const auto acceptance = runtime->RegisterTask({
        .task_id = RequireTaskId("timer-callback"),
        .trigger_at = trigger_at,
        .callback = [callback_done](const TaskId&, TriggerAt) { xSemaphoreGive(callback_done); },
        .on_result =
            [&registration_applied](RegisterTaskResult result) {
                registration_applied.store(result == RegisterTaskResult::kRegistered);
            },
    });

    TEST_ASSERT_EQUAL(static_cast<int>(CommandAcceptance::kAccepted), static_cast<int>(acceptance));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(callback_done, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_TRUE(registration_applied.load());
    vSemaphoreDelete(callback_done);
}
