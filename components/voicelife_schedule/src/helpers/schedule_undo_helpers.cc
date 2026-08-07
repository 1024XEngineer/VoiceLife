#include "schedule_undo_helpers.h"

#include <utility>

#include "../mock/schedule_mock_data.h"

namespace voicelife::schedule {
namespace {

/**
 * @brief 判断目标操作执行后是否允许日程不存在。
 * @param type 目标操作类型。
 * @return 删除或撤销操作允许日程不存在时返回 true。
 */
bool AllowsMissingCurrentSchedule(ScheduleOperationType type) {
    return type == ScheduleOperationType::kDelete || type == ScheduleOperationType::kUndo;
}

}  // namespace

Status ValidateUndoScheduleOperationCommand(const UndoScheduleOperationCommand& command) {
    if (command.operation_id <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "操作记录 ID 必须大于 0");
    }
    return Status::Ok();
}

Result<AppliedScheduleUndo> ApplyMockScheduleUndo(const OperationRecord& operation) {
    if (operation.previous.has_value() && operation.previous->id != operation.schedule_id) {
        return Result<AppliedScheduleUndo>::Failure(ErrorCode::kInternal, "操作记录的日程快照与目标 ID 不一致");
    }

    std::optional<Schedule> before;
    const Result<Schedule> current = FindMockScheduleById(operation.schedule_id);
    if (current.ok()) {
        before = current.value;
    } else if (current.status.code != ErrorCode::kNotFound || !AllowsMissingCurrentSchedule(operation.type)) {
        return Result<AppliedScheduleUndo>::Failure(current.status.code, current.status.message);
    }

    std::optional<Schedule> after;
    switch (operation.type) {
        case ScheduleOperationType::kCreate: {
            const Result<Schedule> removed = RemoveMockSchedule(operation.schedule_id);
            if (!removed.ok()) {
                return Result<AppliedScheduleUndo>::Failure(removed.status.code, removed.status.message);
            }
            break;
        }
        case ScheduleOperationType::kUpdate:
        case ScheduleOperationType::kDelete:
            if (!operation.previous.has_value()) {
                return Result<AppliedScheduleUndo>::Failure(ErrorCode::kInternal, "操作记录缺少可恢复的日程快照");
            }
            if (const Result<Schedule> restored = RestoreMockSchedule(*operation.previous); restored.ok()) {
                after = restored.value;
            } else {
                return Result<AppliedScheduleUndo>::Failure(restored.status.code, restored.status.message);
            }
            break;
        case ScheduleOperationType::kUndo:
            if (operation.previous.has_value()) {
                if (const Result<Schedule> restored = RestoreMockSchedule(*operation.previous); restored.ok()) {
                    after = restored.value;
                } else {
                    return Result<AppliedScheduleUndo>::Failure(restored.status.code, restored.status.message);
                }
            } else {
                const Result<Schedule> removed = RemoveMockSchedule(operation.schedule_id);
                if (!removed.ok()) {
                    return Result<AppliedScheduleUndo>::Failure(removed.status.code, removed.status.message);
                }
            }
            break;
        default:
            return Result<AppliedScheduleUndo>::Failure(ErrorCode::kInternal, "操作记录包含不支持的类型");
    }

    return Result<AppliedScheduleUndo>::Success({.before = std::move(before), .after = std::move(after)});
}

Status RollbackMockScheduleUndo(const AppliedScheduleUndo& applied) {
    if (applied.before.has_value()) return RestoreMockSchedule(*applied.before).status;
    if (!applied.after.has_value()) return Status::Ok();
    return RemoveMockSchedule(applied.after->id).status;
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
