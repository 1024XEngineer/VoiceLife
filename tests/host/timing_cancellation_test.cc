#include <chrono>
#include <memory>
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

void AppliesCancellationOnlyWhenRunnerConsumesIt() {
    InMemoryTimingTaskRunner runner;
    TimingTaskService& service = runner;
    const auto trigger_at = TriggerAt{24h};
    service.RegisterTask(Registration(RequireTaskId("task-1"), trigger_at));
    runner.ProcessPendingCommands(TriggerAt{1h});
    std::optional<CancelTaskResult> final_result;

    const auto acceptance = service.CancelTask({
        .task_id = RequireTaskId("task-1"),
        .on_result = [&final_result](CancelTaskResult result) { final_result = result; },
    });

    Check(acceptance == CommandAcceptance::kAccepted, "cancellation command should be accepted immediately");
    Check(!final_result.has_value(), "cancellation result should wait for Runner consumption");
    Check(runner.NextWakeAt() == trigger_at, "accepted cancellation should not modify the pending registry");

    Check(runner.ProcessPendingCommands(TriggerAt{2h}) == 1, "Runner should consume the queued cancellation");
    Check(final_result == CancelTaskResult::kCancelled, "Runner should report a pending task as cancelled");
    Check(!runner.NextWakeAt().has_value(), "cancelled task should no longer become the next wake time");
}

void ReportsMissingTaskWithoutChangingNextWakeTime() {
    InMemoryTimingTaskRunner runner;
    const auto existing_trigger_at = TriggerAt{24h};
    runner.RegisterTask(Registration(RequireTaskId("task-1"), existing_trigger_at));
    runner.ProcessPendingCommands(TriggerAt{1h});
    std::optional<CancelTaskResult> final_result;

    runner.CancelTask({
        .task_id = RequireTaskId("missing-task"),
        .on_result = [&final_result](CancelTaskResult result) { final_result = result; },
    });

    Check(runner.ProcessPendingCommands(TriggerAt{2h}) == 1, "Runner should consume the missing-task cancellation");
    Check(final_result == CancelTaskResult::kNotFound, "Runner should report an unknown task as not found");
    Check(runner.NextWakeAt() == existing_trigger_at, "missing-task cancellation should preserve the next wake time");
}

void ReportsRepeatedCancellationAsNotFound() {
    InMemoryTimingTaskRunner runner;
    runner.RegisterTask(Registration(RequireTaskId("task-1"), TriggerAt{24h}));
    runner.ProcessPendingCommands(TriggerAt{1h});
    std::optional<CancelTaskResult> first_result;
    std::optional<CancelTaskResult> second_result;
    runner.CancelTask({
        .task_id = RequireTaskId("task-1"),
        .on_result = [&first_result](CancelTaskResult result) { first_result = result; },
    });
    runner.CancelTask({
        .task_id = RequireTaskId("task-1"),
        .on_result = [&second_result](CancelTaskResult result) { second_result = result; },
    });

    Check(runner.ProcessPendingCommands(TriggerAt{2h}) == 2, "Runner should consume both cancellation commands");
    Check(first_result == CancelTaskResult::kCancelled, "first cancellation should cancel the pending task");
    Check(second_result == CancelTaskResult::kNotFound, "repeated cancellation should report the task as not found");
    Check(!runner.NextWakeAt().has_value(), "repeated cancellation should not restore the cancelled task");
}

void KeepsCancelledTaskIdReservedWhileAllowingNewTaskId() {
    InMemoryTimingTaskRunner runner;
    runner.RegisterTask(Registration(RequireTaskId("task-1"), TriggerAt{24h}));
    runner.ProcessPendingCommands(TriggerAt{1h});
    runner.CancelTask({
        .task_id = RequireTaskId("task-1"),
        .on_result = [](CancelTaskResult) {},
    });
    runner.ProcessPendingCommands(TriggerAt{2h});
    std::optional<RegisterTaskResult> reused_id_result;
    std::optional<RegisterTaskResult> new_id_result;

    runner.RegisterTask(Registration(RequireTaskId("task-1"), TriggerAt{3h},
                                     [&reused_id_result](RegisterTaskResult result) { reused_id_result = result; }));
    runner.RegisterTask(Registration(RequireTaskId("task-2"), TriggerAt{4h},
                                     [&new_id_result](RegisterTaskResult result) { new_id_result = result; }));

    Check(runner.ProcessPendingCommands(TriggerAt{2h}) == 2, "Runner should consume both replacement registrations");
    Check(reused_id_result == RegisterTaskResult::kDuplicate, "cancelled task id should remain permanently reserved");
    Check(new_id_result == RegisterTaskResult::kRegistered, "a new task id should register after cancellation");
    Check(runner.NextWakeAt() == TriggerAt{4h}, "only the new task id should contribute to the next wake time");
}

void AppliesRegistrationAndCancellationInAcceptanceOrder() {
    InMemoryTimingTaskRunner register_then_cancel_runner;
    std::optional<RegisterTaskResult> first_registration_result;
    std::optional<CancelTaskResult> following_cancellation_result;
    register_then_cancel_runner.RegisterTask(
        Registration(RequireTaskId("task-1"), TriggerAt{24h},
                     [&first_registration_result](RegisterTaskResult result) { first_registration_result = result; }));
    register_then_cancel_runner.CancelTask({
        .task_id = RequireTaskId("task-1"),
        .on_result =
            [&following_cancellation_result](CancelTaskResult result) { following_cancellation_result = result; },
    });

    Check(register_then_cancel_runner.ProcessPendingCommands(TriggerAt{1h}) == 2,
          "Runner should consume registration followed by cancellation in one cycle");
    Check(first_registration_result == RegisterTaskResult::kRegistered,
          "registration accepted first should be applied first");
    Check(following_cancellation_result == CancelTaskResult::kCancelled,
          "cancellation accepted second should cancel the newly registered task");
    Check(!register_then_cancel_runner.NextWakeAt().has_value(),
          "register-then-cancel order should leave no pending wake time");

    InMemoryTimingTaskRunner cancel_then_register_runner;
    std::optional<CancelTaskResult> first_cancellation_result;
    std::optional<RegisterTaskResult> following_registration_result;
    cancel_then_register_runner.CancelTask({
        .task_id = RequireTaskId("task-2"),
        .on_result = [&first_cancellation_result](CancelTaskResult result) { first_cancellation_result = result; },
    });
    cancel_then_register_runner.RegisterTask(Registration(
        RequireTaskId("task-2"), TriggerAt{30h},
        [&following_registration_result](RegisterTaskResult result) { following_registration_result = result; }));

    Check(cancel_then_register_runner.ProcessPendingCommands(TriggerAt{1h}) == 2,
          "Runner should consume cancellation followed by registration in one cycle");
    Check(first_cancellation_result == CancelTaskResult::kNotFound,
          "cancellation accepted first should run before registration creates the task");
    Check(following_registration_result == RegisterTaskResult::kRegistered,
          "registration accepted second should create the task after the missing cancellation");
    Check(cancel_then_register_runner.NextWakeAt() == TriggerAt{30h},
          "cancel-then-register order should leave the new task pending");
}

void ReleasesRuntimeCallbackAfterCancellation() {
    InMemoryTimingTaskRunner runner;
    auto callback_resource = std::make_shared<int>(1);
    std::weak_ptr<int> callback_resource_observer = callback_resource;
    runner.RegisterTask({
        .task_id = RequireTaskId("task-1"),
        .trigger_at = TriggerAt{24h},
        .callback = [callback_resource](const TaskId&, TriggerAt) {},
        .on_result = [](RegisterTaskResult) {},
    });
    runner.ProcessPendingCommands(TriggerAt{1h});
    callback_resource.reset();
    Check(!callback_resource_observer.expired(), "pending task should retain its runtime callback resources");

    runner.CancelTask({
        .task_id = RequireTaskId("task-1"),
        .on_result = [](CancelTaskResult) {},
    });
    runner.ProcessPendingCommands(TriggerAt{2h});

    Check(callback_resource_observer.expired(), "cancelled task should release its runtime callback resources");
}

}  // namespace

int main() {
    AppliesCancellationOnlyWhenRunnerConsumesIt();
    ReportsMissingTaskWithoutChangingNextWakeTime();
    ReportsRepeatedCancellationAsNotFound();
    KeepsCancelledTaskIdReservedWhileAllowingNewTaskId();
    AppliesRegistrationAndCancellationInAcceptanceOrder();
    ReleasesRuntimeCallbackAfterCancellation();
    return 0;
}
