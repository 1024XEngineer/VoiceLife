#include "voicelife/schedule/schedule_operation_service.h"

#include <chrono>
#include <utility>

#include "../helpers/schedule_create_helpers.h"
#include "../helpers/schedule_operation_helpers.h"
#include "../helpers/schedule_undo_helpers.h"

namespace voicelife::schedule {
namespace {

DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

}  // namespace

ScheduleOperationService::ScheduleOperationService(ScheduleOperationRepository& operation_repository)
    : operation_repository_(operation_repository) {}

// 记录操作：先做参数校验，再组装待落库记录，最后返回仓储补充过 ID 和时间的完整数据。
RecordScheduleOperationResult ScheduleOperationService::record_schedule_operation(
    const RecordScheduleOperationCommand& command) {
    const Status validation = ValidateRecordScheduleOperationCommand(command);
    if (!validation.ok()) return InvalidRecordScheduleOperationResult(validation.message);

    OperationRecord operation{
        .id = 0,
        .type = command.type,
        .schedule_id = command.schedule_id,
        .schedule_event = TrimScheduleText(command.schedule_event),
        .operated_at = {},
        .previous = command.previous,
    };

    const Result<OperationRecord> recorded = operation_repository_.InsertOperation(operation);
    if (!recorded.ok()) {
        return {.result = CommandResult<std::optional<OperationRecord>>::Failure(recorded.status)};
    }

    return {.result = CommandResult<std::optional<OperationRecord>>::Success(recorded.value)};
}

// 查询最近可撤销操作，时间窗口和排序交给操作仓储处理。
QueryRecentScheduleOperationResult ScheduleOperationService::query_recent_schedule_operation() const {
    const Result<std::vector<OperationRecord>> loaded = operation_repository_.FindRecentOperations(Now());
    if (!loaded.ok()) {
        return {.result = CommandResult<std::vector<OperationRecord>>::Failure(loaded.status)};
    }

    return {.result = CommandResult<std::vector<OperationRecord>>::Success(*loaded.value)};
}

// 撤销操作：先做入参校验，再由仓储原子完成撤销并返回恢复后的业务结果。
UndoScheduleOperationResult ScheduleOperationService::undo_schedule_operation(
    const UndoScheduleOperationCommand& command) {
    const Status validation = ValidateUndoScheduleOperationCommand(command);
    if (!validation.ok()) return FailedUndoScheduleOperationResult(validation);

    const Result<UndoOperationResult> undone = operation_repository_.UndoOperation(command.operation_id, Now());
    if (!undone.ok()) return FailedUndoScheduleOperationResult(undone.status);

    return {.result = CommandResult<std::optional<UndoOperationResult>>::Success(undone.value)};
}

}  // namespace voicelife::schedule
