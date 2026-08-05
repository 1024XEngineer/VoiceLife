#include <utility>

#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {

Status TimingPolicy::Validate(const RegisterTimingTaskCommand& command) const {
    if (command.schedule_id.empty() || command.starts_at <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "定时任务缺少日程或时间");
    }
    return Status::Ok();
}

Result<TimingTask> TimingPolicy::Register(const RegisterTimingTaskCommand& command, std::string task_id,
                                          int64_t now) const {
    const Status validation = Validate(command);
    if (!validation.ok() || task_id.empty()) {
        return Result<TimingTask>::Failure(ErrorCode::kInvalidArgument, "定时任务缺少日程、时间或任务标识");
    }
    TimingTask task{
        .id = std::move(task_id),
        .schedule_id = command.schedule_id,
        .start_at = command.starts_at,
        .next_trigger_at = command.starts_at,
        .time_zone = command.time_zone,
        .status = TimingTaskStatus::kActive,
        .created_at = now,
    };
    return Result<TimingTask>::Success(std::move(task));
}

}  // namespace voicelife::timing
