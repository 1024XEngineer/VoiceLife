#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "support/test_support.h"
#include "voicelife/timing/timing_task.h"

using voicelife::test::Check;
using namespace std::chrono_literals;
using namespace voicelife::timing;

namespace {

TaskId RequireTaskId(std::string value) {
    auto task_id = TaskId::Create(std::move(value));
    Check(task_id.has_value(), "test task id should be valid");
    return std::move(*task_id);
}

RegisterTaskCommand Registration(
    TaskId task_id, TriggerAt trigger_at, RegisterTaskResultCallback on_result = [](RegisterTaskResult) {}) {
    return {
        .task_id = std::move(task_id),
        .trigger_at = trigger_at,
        .callback = [](const TaskId&, TriggerAt) {},
        .on_result = std::move(on_result),
    };
}

void RejectsEmptyTaskId() {
    Check(!TaskId::Create("").has_value(), "empty task id should be rejected before registration");
    const auto task_id = TaskId::Create("opaque/task-1");
    Check(task_id.has_value() && task_id->Value() == "opaque/task-1", "task id should preserve its opaque value");
}

void AppliesRegistrationOnlyWhenRunnerConsumesIt() {
    InMemoryTimingTaskRunner runner;
    const auto trigger_at = TriggerAt{24h};
    const auto registered_at = TriggerAt{1h};
    std::optional<RegisterTaskResult> final_result;

    const auto acceptance = runner.RegisterTask(Registration(
        RequireTaskId("task-1"), trigger_at, [&final_result](RegisterTaskResult result) { final_result = result; }));

    Check(acceptance == CommandAcceptance::kAccepted, "registration command should be accepted immediately");
    Check(!final_result.has_value(), "registration result should wait for Runner consumption");
    Check(!runner.NextWakeAt().has_value(), "accepted command should not modify the pending registry");

    Check(runner.ProcessPendingRegistrations(registered_at) == 1, "Runner should consume the queued registration");
    Check(final_result == RegisterTaskResult::kRegistered, "Runner should report a new task as registered");
    Check(runner.NextWakeAt() == trigger_at, "registered task should become the next wake time");
}

void ReportsDuplicateWithoutOverwritingOriginalTask() {
    InMemoryTimingTaskRunner runner;
    const auto original_trigger_at = TriggerAt{24h};
    const auto registered_at = TriggerAt{1h};
    runner.RegisterTask(Registration(RequireTaskId("task-1"), original_trigger_at));
    runner.ProcessPendingRegistrations(registered_at);
    std::optional<RegisterTaskResult> duplicate_result;

    const auto acceptance = runner.RegisterTask(
        Registration(RequireTaskId("task-1"), TriggerAt{2h},
                     [&duplicate_result](RegisterTaskResult result) { duplicate_result = result; }));

    Check(acceptance == CommandAcceptance::kAccepted,
          "duplicate registration should still be accepted as an asynchronous command");
    Check(!duplicate_result.has_value(), "duplicate result should wait for Runner consumption");
    Check(runner.ProcessPendingRegistrations(registered_at) == 1, "Runner should consume the duplicate command");
    Check(duplicate_result == RegisterTaskResult::kDuplicate, "Runner should report a reused task id as duplicate");
    Check(runner.NextWakeAt() == original_trigger_at, "duplicate registration should not overwrite the pending task");
}

void FindsEarliestPendingTrigger() {
    InMemoryTimingTaskRunner runner;
    runner.RegisterTask(Registration(RequireTaskId("task-1"), TriggerAt{24h}));
    runner.RegisterTask(Registration(RequireTaskId("task-2"), TriggerAt{30h}));
    runner.RegisterTask(Registration(RequireTaskId("task-3"), TriggerAt{3h}));

    Check(runner.ProcessPendingRegistrations(TriggerAt{1h}) == 3,
          "Runner should consume all registrations waiting at the start of a cycle");
    Check(runner.NextWakeAt() == TriggerAt{3h}, "next wake time should be the earliest pending trigger");
}

void DefersRegistrationsSubmittedByResultCallback() {
    InMemoryTimingTaskRunner runner;
    std::optional<RegisterTaskResult> deferred_result;
    runner.RegisterTask(
        Registration(RequireTaskId("task-1"), TriggerAt{2h}, [&runner, &deferred_result](RegisterTaskResult) {
            runner.RegisterTask(
                Registration(RequireTaskId("task-2"), TriggerAt{1h},
                             [&deferred_result](RegisterTaskResult result) { deferred_result = result; }));
        }));

    Check(runner.ProcessPendingRegistrations(TriggerAt{}) == 1,
          "Runner cycle should consume only registrations queued when the cycle began");
    Check(!deferred_result.has_value(), "registration submitted by a result callback should wait for the next cycle");
    Check(runner.NextWakeAt() == TriggerAt{2h}, "deferred registration should not affect the current cycle");

    Check(runner.ProcessPendingRegistrations(TriggerAt{}) == 1,
          "next Runner cycle should consume the registration submitted by the callback");
    Check(deferred_result == RegisterTaskResult::kRegistered,
          "deferred registration should complete on the next cycle");
    Check(runner.NextWakeAt() == TriggerAt{1h}, "deferred registration should affect wake time after it is applied");
}

void RejectsNestedRunnerConsumption() {
    InMemoryTimingTaskRunner runner;
    std::optional<size_t> nested_processed_count;
    std::optional<RegisterTaskResult> second_result;
    runner.RegisterTask(
        Registration(RequireTaskId("task-1"), TriggerAt{2h}, [&runner, &nested_processed_count](RegisterTaskResult) {
            nested_processed_count = runner.ProcessPendingRegistrations(TriggerAt{});
        }));
    runner.RegisterTask(Registration(RequireTaskId("task-2"), TriggerAt{1h},
                                     [&second_result](RegisterTaskResult result) { second_result = result; }));

    Check(runner.ProcessPendingRegistrations(TriggerAt{}) == 2,
          "outer Runner cycle should retain ownership of its queued registrations");
    Check(nested_processed_count == 0, "nested Runner consumption should be rejected");
    Check(second_result == RegisterTaskResult::kRegistered,
          "outer Runner cycle should still apply commands after the reentrant callback");
    Check(runner.NextWakeAt() == TriggerAt{1h}, "reentrant callback should not corrupt the pending registry");
}

}  // namespace

int main() {
    RejectsEmptyTaskId();
    AppliesRegistrationOnlyWhenRunnerConsumesIt();
    ReportsDuplicateWithoutOverwritingOriginalTask();
    FindsEarliestPendingTrigger();
    DefersRegistrationsSubmittedByResultCallback();
    RejectsNestedRunnerConsumption();
    return 0;
}
