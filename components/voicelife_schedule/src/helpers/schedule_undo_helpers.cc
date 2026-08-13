#include "schedule_undo_helpers.h"

#include <utility>

namespace voicelife::schedule {

Status ValidateUndoScheduleOperationCommand(const UndoScheduleOperationCommand& command) {
    if (command.operation_id <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "操作记录 ID 必须大于 0");
    }
    return Status::Ok();
}

UndoScheduleOperationResult FailedUndoScheduleOperationResult(Status status) {
    return {
        .status = status,
        .undone = false,
        .operation = std::nullopt,
        .schedule = std::nullopt,
        .error = std::move(status.message),
    };
}

}  // namespace voicelife::schedule
