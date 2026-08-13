#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

RegisterTaskCommand Registration(TaskId task_id, TriggerAt trigger_at, TaskCallback callback) {
    return {
        .task_id = std::move(task_id),
        .trigger_at = trigger_at,
        .callback = std::move(callback),
        .on_result = [](RegisterTaskResult) {},
    };
}

void LeavesFutureTaskPendingAfterEmptyDueBatch() {
    InMemoryTimingTaskRunner runner;
    const auto trigger_at = TriggerAt{24h};
    std::optional<RegisterTaskResult> registration_result;
    bool callback_invoked = false;
    runner.RegisterTask({
        .task_id = RequireTaskId("task-1"),
        .trigger_at = trigger_at,
        .callback = [&callback_invoked](const TaskId&, TriggerAt) { callback_invoked = true; },
        .on_result = [&registration_result](RegisterTaskResult result) { registration_result = result; },
    });

    const auto result = runner.RunDueTasks(TriggerAt{1h});

    Check(registration_result == RegisterTaskResult::kRegistered,
          "RunDueTasks should apply accepted commands before forming the due batch");
    Check(!callback_invoked, "future task should not run in an empty due batch");
    Check(result.processed_count == 0, "empty due batch should process no task");
    Check(result.skipped_count == 0, "empty due batch should skip no task");
    Check(result.next_wake_at == trigger_at, "empty due batch should return the future task as next wake time");
}

void ExecutesTaskAtDueBoundary() {
    InMemoryTimingTaskRunner runner;
    const auto trigger_at = TriggerAt{2h};
    std::optional<std::string> callback_task_id;
    std::optional<TriggerAt> callback_trigger_at;
    runner.RegisterTask({
        .task_id = RequireTaskId("task-1"),
        .trigger_at = trigger_at,
        .callback =
            [&callback_task_id, &callback_trigger_at](const TaskId& task_id, TriggerAt due_at) {
                callback_task_id = task_id.Value();
                callback_trigger_at = due_at;
            },
        .on_result = [](RegisterTaskResult) {},
    });

    const auto result = runner.RunDueTasks(trigger_at);

    Check(callback_task_id == "task-1", "due callback should receive the registered task id");
    Check(callback_trigger_at == trigger_at, "due callback should receive the registered trigger time");
    Check(result.processed_count == 1, "task at the due boundary should be processed");
    Check(result.skipped_count == 0, "processed task should not be reported as skipped");
    Check(!result.next_wake_at.has_value(), "completed task should no longer produce a next wake time");
}

void ExecutesAllDueTasksInStableOrder() {
    InMemoryTimingTaskRunner runner;
    std::vector<std::string> callback_order;
    const auto record_id = [&callback_order](const TaskId& task_id, TriggerAt) {
        callback_order.push_back(task_id.Value());
    };
    runner.RegisterTask(Registration(RequireTaskId("task-c"), TriggerAt{3h}, record_id));
    runner.RegisterTask(Registration(RequireTaskId("task-b"), TriggerAt{1h}, record_id));
    runner.RegisterTask(Registration(RequireTaskId("task-a"), TriggerAt{1h}, record_id));
    runner.RegisterTask(Registration(RequireTaskId("task-future"), TriggerAt{4h}, record_id));

    const auto result = runner.RunDueTasks(TriggerAt{3h});

    Check(callback_order == std::vector<std::string>{"task-a", "task-b", "task-c"},
          "due tasks should run by trigger time and then task id");
    Check(result.processed_count == 3, "Runner should process every task in the due batch");
    Check(result.skipped_count == 0, "stable due batch should skip no task");
    Check(result.next_wake_at == TriggerAt{4h}, "future task should remain as the next wake time");
}

void DoesNotRepeatCompletedTaskOnLaterRun() {
    InMemoryTimingTaskRunner runner;
    size_t callback_count = 0;
    const auto trigger_at = TriggerAt{2h};
    runner.RegisterTask(Registration(RequireTaskId("task-1"), trigger_at,
                                     [&callback_count](const TaskId&, TriggerAt) { ++callback_count; }));

    const auto first_result = runner.RunDueTasks(trigger_at);
    const auto second_result = runner.RunDueTasks(trigger_at);

    Check(callback_count == 1, "completed task callback should run exactly once");
    Check(first_result.processed_count == 1, "first run should process the due task");
    Check(second_result.processed_count == 0, "later run should not process the completed task again");
    Check(second_result.skipped_count == 0, "later run should not report a completed task as skipped");
    Check(!second_result.next_wake_at.has_value(), "later run should keep the completed task out of wake scheduling");
}

void AppliesAcceptedCancellationBeforeFormingDueBatch() {
    InMemoryTimingTaskRunner runner;
    bool callback_invoked = false;
    std::optional<CancelTaskResult> cancellation_result;
    runner.RegisterTask(Registration(RequireTaskId("task-1"), TriggerAt{1h},
                                     [&callback_invoked](const TaskId&, TriggerAt) { callback_invoked = true; }));
    runner.ProcessPendingCommands(TriggerAt{});
    runner.CancelTask({
        .task_id = RequireTaskId("task-1"),
        .on_result = [&cancellation_result](CancelTaskResult result) { cancellation_result = result; },
    });

    const auto result = runner.RunDueTasks(TriggerAt{2h});

    Check(cancellation_result == CancelTaskResult::kCancelled,
          "RunDueTasks should apply accepted cancellation before forming the due batch");
    Check(!callback_invoked, "task cancelled before batch formation should not invoke its callback");
    Check(result.processed_count == 0, "cancelled task should not count as processed");
    Check(result.skipped_count == 0, "task removed before batch formation should not count as skipped");
    Check(!result.next_wake_at.has_value(), "cancelled task should not remain as the next wake time");
}

void ContinuesDueBatchAfterEarlierCallbackReturns() {
    InMemoryTimingTaskRunner runner;
    std::vector<std::string> callback_events;
    runner.RegisterTask(
        Registration(RequireTaskId("task-1"), TriggerAt{1h}, [&callback_events](const TaskId&, TriggerAt) {
            callback_events.push_back("task-1-start");
            callback_events.push_back("task-1-end");
        }));
    runner.RegisterTask(
        Registration(RequireTaskId("task-2"), TriggerAt{1h},
                     [&callback_events](const TaskId&, TriggerAt) { callback_events.push_back("task-2"); }));

    const auto result = runner.RunDueTasks(TriggerAt{1h});

    Check(callback_events == std::vector<std::string>{"task-1-start", "task-1-end", "task-2"},
          "later due task should run after the earlier callback returns");
    Check(result.processed_count == 2, "Runner should finish the due batch after an earlier callback returns");
}

}  // namespace

int main() {
    LeavesFutureTaskPendingAfterEmptyDueBatch();
    ExecutesTaskAtDueBoundary();
    ExecutesAllDueTasksInStableOrder();
    DoesNotRepeatCompletedTaskOnLaterRun();
    AppliesAcceptedCancellationBeforeFormingDueBatch();
    ContinuesDueBatchAfterEarlierCallbackReturns();
    return 0;
}
