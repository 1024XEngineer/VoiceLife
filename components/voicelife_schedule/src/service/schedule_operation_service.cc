#include "voicelife/schedule/schedule_operation_service.h"

#include <utility>

#include "../helpers/schedule_operation_helpers.h"

namespace voicelife::schedule {

ScheduleOperationService::ScheduleOperationService(ScheduleOperationRepository& operation_repository)
    : operation_repository_(operation_repository) {}

// 记录操作：先做参数校验，再组装待落库记录，最后返回仓储补充过 ID 和时间的完整数据。
RecordOperationResult ScheduleOperationService::record_operation(const RecordOperationCommand& command) {
    const Status validation = ValidateRecordOperationCommand(command);
    if (!validation.ok()) return InvalidRecordOperationResult(validation.message);

    OperationRecord operation{
        .id = 0,
        .entity_type = command.entity_type,
        .type = command.type,
        .entity_id = command.entity_id,
        .operated_at = {},
        .label = command.label,
        .before = command.before,
    };

    const Result<OperationRecord> recorded = operation_repository_.InsertOperation(operation);
    if (!recorded.ok()) {
        return {.result = CommandResult<std::optional<OperationRecord>>::Failure(recorded.status)};
    }

    return {.result = CommandResult<std::optional<OperationRecord>>::Success(recorded.value)};
}

// 查询操作：先做参数校验，再查询满足条件的记录和总条数。
QueryOperationResult ScheduleOperationService::query_operations(const QueryOperationCommand& command) const {
    const Status validation = ValidateQueryOperationCommand(command);
    if (!validation.ok()) return InvalidQueryOperationResult(validation.message);

    const Result<std::vector<OperationRecord>> loaded = operation_repository_.FindOperations(command);
    if (!loaded.ok()) {
        return {.result = CommandResult<std::vector<OperationRecord>>::Failure(loaded.status)};
    }

    const Result<int64_t> counted = operation_repository_.CountOperations(command);
    if (!counted.ok()) {
        return {.result = CommandResult<std::vector<OperationRecord>>::Failure(counted.status)};
    }

    return {.result = CommandResult<std::vector<OperationRecord>>::Success(*loaded.value), .total = *counted.value};
}

}  // namespace voicelife::schedule
