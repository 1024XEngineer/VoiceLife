#include "mapping/operation_row_mapper.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace voicelife::storage_sqlite::mapping {
namespace {

/** @brief 为操作字段绑定错误补充字段名。 @param status 底层状态。 @param field 字段名。 @return 带上下文的状态。 */
Status WithField(Status status, const char* field) {
    if (status.ok()) return status;
    return Status::Error(status.code, std::string("绑定操作字段失败：") + field + "；" + status.message);
}

/** @brief 判断实体类型是否有效。 @param value 数据库整数。 @return 有效时返回 true。 */
bool IsValidEntityType(int value) {
    return value == static_cast<int>(schedule::OperationEntityType::kSchedule) ||
           value == static_cast<int>(schedule::OperationEntityType::kRule) ||
           value == static_cast<int>(schedule::OperationEntityType::kException);
}

/** @brief 判断操作类型是否有效。 @param value 数据库整数。 @return 有效时返回 true。 */
bool IsValidOperationType(int value) {
    return value == static_cast<int>(schedule::ScheduleOperationType::kCreate) ||
           value == static_cast<int>(schedule::ScheduleOperationType::kUpdate) ||
           value == static_cast<int>(schedule::ScheduleOperationType::kDelete);
}

}  // namespace

Status BindOperation(SqliteStatement& statement, const schedule::OperationRecord& operation) {
    Status status = WithField(statement.BindInt(1, static_cast<int>(operation.entity_type)), "entity_type");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt(2, static_cast<int>(operation.type)), "type");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(3, operation.entity_id), "entity_id");
    if (!status.ok()) return status;
    status = WithField(statement.BindText(4, operation.label), "label");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(5, operation.operated_at.time_since_epoch().count()), "operated_at");
    if (!status.ok()) return status;
    if (operation.before.has_value()) {
        return WithField(statement.BindText(6, *operation.before), "before");
    }
    return WithField(statement.BindNull(6), "before");
}

Result<schedule::OperationRecord> ReadOperation(const SqliteStatement& statement) {
    const int entity_type = statement.ColumnInt(1);
    if (!IsValidEntityType(entity_type)) {
        return Result<schedule::OperationRecord>::Failure(ErrorCode::kInternal, "数据库中的实体类型无效");
    }
    const int type = statement.ColumnInt(2);
    if (!IsValidOperationType(type)) {
        return Result<schedule::OperationRecord>::Failure(ErrorCode::kInternal, "数据库中的操作类型无效");
    }
    if (statement.IsNull(0) || statement.ColumnInt64(3) <= 0 || statement.IsNull(4) || statement.IsNull(5)) {
        return Result<schedule::OperationRecord>::Failure(ErrorCode::kInternal, "数据库中的操作字段为空");
    }
    const bool before_present = !statement.IsNull(6);
    if ((type == static_cast<int>(schedule::ScheduleOperationType::kCreate)) && before_present) {
        return Result<schedule::OperationRecord>::Failure(ErrorCode::kInternal, "创建操作不应携带 before 快照");
    }
    if (type != static_cast<int>(schedule::ScheduleOperationType::kCreate) && !before_present) {
        return Result<schedule::OperationRecord>::Failure(ErrorCode::kInternal, "修改和删除操作必须携带 before 快照");
    }

    schedule::OperationRecord operation{
        .id = statement.ColumnInt64(0),
        .entity_type = static_cast<schedule::OperationEntityType>(entity_type),
        .type = static_cast<schedule::ScheduleOperationType>(type),
        .entity_id = statement.ColumnInt64(3),
        .operated_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(5)}},
        .label = statement.ColumnText(4),
        .before = before_present ? std::optional<std::string>(statement.ColumnText(6)) : std::nullopt,
    };
    return Result<schedule::OperationRecord>::Success(std::move(operation));
}

}  // namespace voicelife::storage_sqlite::mapping
