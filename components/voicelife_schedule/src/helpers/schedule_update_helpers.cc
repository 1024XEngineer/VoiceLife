#include "schedule_update_helpers.h"

#include <utility>

namespace voicelife::schedule {

// 构造修改日程的参数错误结果，确保错误状态、空消息和空冲突列表结构一致。
UpdateScheduleResult InvalidUpdateScheduleResult(std::string error) {
    const Status status = Status::Error(ErrorCode::kInvalidArgument, error);
    return {
        .result = CommandResult<std::optional<Schedule>>::Failure(status),
        .message = {},
        .conflicts = {},
    };
}

}  // namespace voicelife::schedule
