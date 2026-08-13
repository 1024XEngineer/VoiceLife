#include "schedule_undo_helpers.h"

#include <utility>

namespace voicelife::schedule {

// 撤销入口先校验操作记录 ID，后续仓储层再负责查找、权限/窗口判断和执行撤销。
Status ValidateUndoScheduleOperationCommand(const UndoScheduleOperationCommand& command) {
    if (command.operation_id <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "操作记录 ID 必须大于 0");
    }
    return Status::Ok();
}

// 统一构造撤销失败结果，避免调用方到处拼装空的撤销数据。
UndoScheduleOperationResult FailedUndoScheduleOperationResult(Status status) {
    return {
        .result = CommandResult<std::optional<UndoOperationResult>>::Failure(status),
    };
}

}  // namespace voicelife::schedule
