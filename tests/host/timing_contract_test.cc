#include <concepts>
#include <type_traits>
#include <utility>

#include "support/test_support.h"
#include "voicelife/timing/timing_task.h"

using voicelife::test::Check;
using namespace voicelife::timing;

template <typename Service>
concept TimingTaskServiceContract =
    requires(Service& service, RegisterTaskCommand register_command, CancelTaskCommand cancel_command) {
        { service.RegisterTask(std::move(register_command)) } -> std::same_as<CommandAcceptance>;
        { service.CancelTask(std::move(cancel_command)) } -> std::same_as<CommandAcceptance>;
    };

static_assert(std::has_virtual_destructor_v<TimingTaskService>);
static_assert(TimingTaskServiceContract<TimingTaskService>);
static_assert(std::same_as<TaskCallback, std::function<void(const TaskId&, TriggerAt)>>);

int main() {
    Check(CanTransition(TaskStatus::kPending, TaskStatus::kExecuting),
          "pending task should be allowed to start executing");
    Check(CanTransition(TaskStatus::kPending, TaskStatus::kCancelled),
          "pending task should be allowed to be cancelled");
    Check(CanTransition(TaskStatus::kExecuting, TaskStatus::kCompleted),
          "executing task should be allowed to complete");
    Check(!CanTransition(TaskStatus::kCompleted, TaskStatus::kExecuting),
          "completed task should not return to executing");
    Check(!CanTransition(TaskStatus::kCancelled, TaskStatus::kExecuting), "cancelled task should not start executing");
    return 0;
}
