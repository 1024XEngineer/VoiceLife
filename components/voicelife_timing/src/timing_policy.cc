#include <utility>

#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {

Result<TimingTask> TimingPolicy::Register(const RegisterTimingTaskCommand& command, std::string task_id,
                                          int64_t now) const {
    if (command.schedule_id.empty() || command.starts_at <= 0 || task_id.empty()) {
        return Result<TimingTask>::Failure(ErrorCode::kInvalidArgument, "定时任务缺少日程、时间或任务标识");
    }
    TimingTask task{
        .id = std::move(task_id),
        .schedule_id = command.schedule_id,
        .next_trigger_at = command.starts_at,
        .time_zone = command.time_zone,
        .status = TimingTaskStatus::kActive,
        .created_at = now,
    };
    return Result<TimingTask>::Success(std::move(task));
}

}  // namespace voicelife::timing
