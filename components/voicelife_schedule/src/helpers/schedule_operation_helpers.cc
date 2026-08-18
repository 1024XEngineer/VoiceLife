#include "schedule_operation_helpers.h"

#include <cstddef>
#include <utility>

#include "schedule_create_helpers.h"

namespace voicelife::schedule {
namespace {

constexpr std::size_t kMaximumLabelLength = 100;
constexpr std::size_t kMaximumSnapshotBytes = 2048;

/** @brief 判断实体类型是否属于日程模块支持的范围。 @param type 待判断的实体类型。 @return 类型合法时返回 true。 */
bool IsSupportedEntityType(OperationEntityType type) {
    switch (type) {
        case OperationEntityType::kSchedule:
        case OperationEntityType::kRule:
        case OperationEntityType::kException:
            return true;
    }
    return false;
}

/** @brief 判断操作类型是否属于日程模块支持的范围。 @param type 待判断的操作类型。 @return 类型合法时返回 true。 */
bool IsSupportedScheduleOperationType(ScheduleOperationType type) {
    switch (type) {
        case ScheduleOperationType::kCreate:
        case ScheduleOperationType::kUpdate:
        case ScheduleOperationType::kDelete:
            return true;
    }
    return false;
}

}  // namespace

Status ValidateRecordOperationCommand(const RecordOperationCommand& command) {
    if (!IsSupportedEntityType(command.entity_type)) {
        return Status::Error(ErrorCode::kInvalidArgument, "实体类型必须为日程、规则或例外");
    }
    if (!IsSupportedScheduleOperationType(command.type)) {
        return Status::Error(ErrorCode::kInvalidArgument, "操作类型必须为创建、修改或删除");
    }
    if (command.entity_id <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "实体 ID 必须大于 0");
    }

    const std::string label = TrimScheduleText(command.label);
    if (label.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "操作对象名称不能为空");
    }
    if (ScheduleTextLength(label) > kMaximumLabelLength) {
        return Status::Error(ErrorCode::kInvalidArgument, "操作对象名称不能超过 100 个字符");
    }

    if (command.type == ScheduleOperationType::kCreate && command.before.has_value()) {
        return Status::Error(ErrorCode::kInvalidArgument, "创建操作不能提供 before 快照");
    }
    if ((command.type == ScheduleOperationType::kUpdate || command.type == ScheduleOperationType::kDelete) &&
        !command.before.has_value()) {
        return Status::Error(ErrorCode::kInvalidArgument, "修改和删除操作必须提供 before 快照");
    }
    if (command.before.has_value() && command.before->size() > kMaximumSnapshotBytes) {
        return Status::Error(ErrorCode::kInvalidArgument, "before 快照不能超过 2048 字节");
    }

    return Status::Ok();
}

RecordOperationResult InvalidRecordOperationResult(std::string error) {
    const Status status = Status::Error(ErrorCode::kInvalidArgument, error);
    return {
        .result = CommandResult<std::optional<OperationRecord>>::Failure(status),
    };
}

Status ValidateQueryOperationCommand(const QueryOperationCommand& command) {
    if (command.entity_id.has_value() && !command.entity_type.has_value()) {
        return Status::Error(ErrorCode::kInvalidArgument, "按实体 ID 查询时必须同时提供实体类型");
    }
    if (command.entity_type.has_value() && !IsSupportedEntityType(*command.entity_type)) {
        return Status::Error(ErrorCode::kInvalidArgument, "实体类型必须为日程、规则或例外");
    }
    if (command.type.has_value() && !IsSupportedScheduleOperationType(*command.type)) {
        return Status::Error(ErrorCode::kInvalidArgument, "操作类型必须为创建、修改或删除");
    }
    if (command.operated_from.has_value() && command.operated_to.has_value() &&
        *command.operated_from > *command.operated_to) {
        return Status::Error(ErrorCode::kInvalidArgument, "查询时间下界不能晚于上界");
    }
    if (command.limit <= 0 || command.limit > 100) {
        return Status::Error(ErrorCode::kInvalidArgument, "limit 必须在 1 到 100 之间");
    }
    if (command.offset < 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "offset 不能小于 0");
    }

    return Status::Ok();
}

QueryOperationResult InvalidQueryOperationResult(std::string error) {
    const Status status = Status::Error(ErrorCode::kInvalidArgument, error);
    return {
        .result = CommandResult<std::vector<OperationRecord>>::Failure(status),
        .total = 0,
    };
}

}  // namespace voicelife::schedule
