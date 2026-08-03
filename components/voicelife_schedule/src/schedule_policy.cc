#include <utility>

#include "voicelife/schedule/schedule.h"

namespace voicelife::schedule {

Result<Schedule> SchedulePolicy::Create(const CreateScheduleCommand& command, std::string schedule_id,
                                        int64_t now) const {
    if (command.request_id.empty()) {
        return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "request_id 不能为空");
    }
    if (command.title.empty() || command.title.size() > 100) {
        return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "标题长度必须在 1 到 100 字节之间");
    }
    if (command.starts_at <= 0) {
        return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "开始时间无效");
    }
    if (command.ends_at != 0 && command.ends_at < command.starts_at) {
        return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "结束时间不能早于开始时间");
    }
    if (schedule_id.empty()) {
        return Result<Schedule>::Failure(ErrorCode::kInternal, "未生成日程标识");
    }

    Schedule schedule{
        .id = std::move(schedule_id),
        .request_id = command.request_id,
        .title = command.title,
        .starts_at = command.starts_at,
        .ends_at = command.ends_at,
        .time_zone = command.time_zone,
        .status = ScheduleStatus::kActive,
        .created_at = now,
    };
    return Result<Schedule>::Success(std::move(schedule));
}

}  // namespace voicelife::schedule
