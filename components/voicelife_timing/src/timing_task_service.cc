#include "voicelife/timing/timing_task_service.h"

#include <utility>

#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {

Result<RegisterTimerTaskResult> DefaultTimingTaskService::RegisterTimerTask(
    const RegisterTimerTaskCommand& command) {
    const RegisterTimingTaskCommand policy_command{
        .schedule_id = command.schedule_id,
        .starts_at = command.start_at,
        .time_zone = command.time_zone,
    };

    auto task = TimingPolicy{}.Register(policy_command, ids_.NextTaskId(), clock_.Now());
    if (!task.ok()) {
        return Result<RegisterTimerTaskResult>::Failure(task.status.code, task.status.message);
    }

    const Status saved = store_.SaveTask(*task.value);
    if (!saved.ok()) {
        return Result<RegisterTimerTaskResult>::Failure(saved.code, saved.message);
    }

    return Result<RegisterTimerTaskResult>::Success({
        .task_id = std::move(task.value->id),
        .status = task.value->status,
        .next_trigger_at = task.value->next_trigger_at,
    });
}

}  // namespace voicelife::timing
