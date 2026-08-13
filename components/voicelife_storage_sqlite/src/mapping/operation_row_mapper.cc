#include "mapping/operation_row_mapper.h"

#include <chrono>
#include <optional>
#include <string>

namespace voicelife::storage_sqlite::mapping {
namespace {

/** @brief 为操作字段绑定错误补充字段名。 @param status 底层状态。 @param field 字段名。 @return 带上下文的状态。 */
Status WithField(Status status, const char* field) {
    if (status.ok()) return status;
    return Status::Error(status.code, std::string("绑定操作字段失败：") + field + "；" + status.message);
}

/**
 * @brief 绑定可空时间列。
 * @param statement SQLite 语句。
 * @param index 参数序号。
 * @param value 可选时间。
 * @param field 字段名。
 * @return 绑定状态。
 */
Status BindOptionalTime(SqliteStatement& statement, int index, const std::optional<schedule::DateTime>& value,
                        const char* field) {
    return WithField(value.has_value() ? statement.BindInt64(index, value->time_since_epoch().count())
                                       : statement.BindNull(index),
                     field);
}

/**
 * @brief 绑定可空整数列。
 * @param statement SQLite 语句。
 * @param index 参数序号。
 * @param value 可选整数。
 * @param field 字段名。
 * @return 绑定状态。
 */
Status BindOptionalInt64(SqliteStatement& statement, int index, const std::optional<int64_t>& value,
                         const char* field) {
    return WithField(value.has_value() ? statement.BindInt64(index, *value) : statement.BindNull(index), field);
}

/**
 * @brief 绑定可空文本列。
 * @param statement SQLite 语句。
 * @param index 参数序号。
 * @param value 可选文本。
 * @param field 字段名。
 * @return 绑定状态。
 */
Status BindOptionalText(SqliteStatement& statement, int index, const std::optional<std::string>& value,
                        const char* field) {
    return WithField(value.has_value() ? statement.BindText(index, *value) : statement.BindNull(index), field);
}

/** @brief 将日程时间列转换为可选领域时间。 @param statement 查询语句。 @param column 列序号。 @return 可选时间。 */
std::optional<schedule::DateTime> ReadOptionalTime(const SqliteStatement& statement, int column) {
    if (statement.IsNull(column)) return std::nullopt;
    return schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(column)}};
}

/** @brief 将文本列转换为可选领域文本。 @param statement 查询语句。 @param column 列序号。 @return 可选文本。 */
std::optional<std::string> ReadOptionalText(const SqliteStatement& statement, int column) {
    if (statement.IsNull(column)) return std::nullopt;
    return statement.ColumnText(column);
}

/** @brief 判断操作类型是否有效。 @param value 数据库整数。 @return 有效时返回 true。 */
bool IsValidOperationType(int value) {
    return value >= static_cast<int>(schedule::ScheduleOperationType::kCreate) &&
           value <= static_cast<int>(schedule::ScheduleOperationType::kUndo);
}

/** @brief 判断日程状态是否有效。 @param value 数据库整数。 @return 有效时返回 true。 */
bool IsValidScheduleStatus(int value) {
    return value == static_cast<int>(schedule::ScheduleStatus::kActive) ||
           value == static_cast<int>(schedule::ScheduleStatus::kCancelled) ||
           value == static_cast<int>(schedule::ScheduleStatus::kCompleted);
}

/**
 * @brief 从操作结果行读取前置日程快照。
 * @param statement 查询语句。
 * @return 空快照或完整日程快照。
 */
Result<std::optional<schedule::Schedule>> ReadPrevious(const SqliteStatement& statement) {
    if (statement.IsNull(6)) {
        for (int column = 7; column <= 15; ++column) {
            if (!statement.IsNull(column)) {
                return Result<std::optional<schedule::Schedule>>::Failure(ErrorCode::kInternal,
                                                                           "操作快照列不一致");
            }
        }
        return Result<std::optional<schedule::Schedule>>::Success(std::nullopt);
    }
    if (statement.IsNull(7) || statement.IsNull(13) || statement.IsNull(14) || statement.IsNull(15)) {
        return Result<std::optional<schedule::Schedule>>::Failure(ErrorCode::kInternal, "操作日程快照字段不完整");
    }
    const int status = statement.ColumnInt(13);
    if (!IsValidScheduleStatus(status)) {
        return Result<std::optional<schedule::Schedule>>::Failure(ErrorCode::kInternal, "操作快照中的日程状态无效");
    }
    schedule::Schedule snapshot{
        .id = statement.ColumnInt64(6),
        .event = statement.ColumnText(7),
        .start_time = ReadOptionalTime(statement, 8),
        .end_time = ReadOptionalTime(statement, 9),
        .location = ReadOptionalText(statement, 10),
        .notes = ReadOptionalText(statement, 11),
        .rule_id = statement.IsNull(12) ? std::nullopt : std::optional<schedule::ScheduleId>(statement.ColumnInt64(12)),
        .status = static_cast<schedule::ScheduleStatus>(status),
        .created_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(14)}},
        .updated_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(15)}},
    };
    if (snapshot.id != statement.ColumnInt64(2)) {
        return Result<std::optional<schedule::Schedule>>::Failure(ErrorCode::kInternal, "操作快照日程 ID 不一致");
    }
    if (snapshot.end_time.has_value() &&
        (!snapshot.start_time.has_value() || snapshot.end_time <= snapshot.start_time)) {
        return Result<std::optional<schedule::Schedule>>::Failure(ErrorCode::kInternal, "操作快照时间范围无效");
    }
    return Result<std::optional<schedule::Schedule>>::Success(std::move(snapshot));
}

}  // namespace

Status BindOperation(SqliteStatement& statement, const schedule::OperationRecord& operation) {
    Status status = WithField(statement.BindInt(1, static_cast<int>(operation.type)), "type");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(2, operation.schedule_id), "schedule_id");
    if (!status.ok()) return status;
    status = WithField(statement.BindText(3, operation.schedule_event), "schedule_event");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(4, operation.operated_at.time_since_epoch().count()), "operated_at");
    if (!status.ok()) return status;

    const std::optional<schedule::Schedule>& previous = operation.previous;
    status = BindOptionalInt64(statement, 5, previous.has_value() ? std::optional<int64_t>(previous->id) : std::nullopt,
                               "previous_id");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, 6, previous.has_value() ? std::optional<std::string>(previous->event)
                                                                  : std::nullopt,
                              "previous_event");
    if (!status.ok()) return status;
    status = BindOptionalTime(statement, 7, previous.has_value() ? previous->start_time : std::nullopt,
                              "previous_start_time");
    if (!status.ok()) return status;
    status = BindOptionalTime(statement, 8, previous.has_value() ? previous->end_time : std::nullopt,
                              "previous_end_time");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, 9, previous.has_value() ? previous->location : std::nullopt,
                              "previous_location");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, 10, previous.has_value() ? previous->notes : std::nullopt, "previous_notes");
    if (!status.ok()) return status;
    status = BindOptionalInt64(statement, 11, previous.has_value() ? previous->rule_id : std::nullopt,
                               "previous_rule_id");
    if (!status.ok()) return status;
    status = WithField(previous.has_value() ? statement.BindInt(12, static_cast<int>(previous->status))
                                             : statement.BindNull(12),
                       "previous_status");
    if (!status.ok()) return status;
    status = BindOptionalTime(statement, 13, previous.has_value() ? std::optional<schedule::DateTime>(previous->created_at)
                                                                  : std::nullopt,
                              "previous_created_at");
    if (!status.ok()) return status;
    return BindOptionalTime(statement, 14,
                            previous.has_value() ? std::optional<schedule::DateTime>(previous->updated_at)
                                                 : std::nullopt,
                            "previous_updated_at");
}

Result<schedule::OperationRecord> ReadOperation(const SqliteStatement& statement) {
    if (!IsValidOperationType(statement.ColumnInt(1))) {
        return Result<schedule::OperationRecord>::Failure(ErrorCode::kInternal, "数据库中的操作类型无效");
    }
    if (statement.IsNull(3) || statement.IsNull(4) || statement.IsNull(5)) {
        return Result<schedule::OperationRecord>::Failure(ErrorCode::kInternal, "数据库中的操作字段为空");
    }
    const int active = statement.ColumnInt(5);
    if (active != 0 && active != 1) {
        return Result<schedule::OperationRecord>::Failure(ErrorCode::kInternal, "数据库中的操作 active 状态无效");
    }
    const Result<std::optional<schedule::Schedule>> previous = ReadPrevious(statement);
    if (!previous.ok()) return Result<schedule::OperationRecord>::Failure(previous.status.code, previous.status.message);
    schedule::OperationRecord operation{
        .id = statement.ColumnInt64(0),
        .type = static_cast<schedule::ScheduleOperationType>(statement.ColumnInt(1)),
        .schedule_id = statement.ColumnInt64(2),
        .schedule_event = statement.ColumnText(3),
        .operated_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(4)}},
        .previous = *previous.value,
    };
    return Result<schedule::OperationRecord>::Success(std::move(operation));
}

Status BindScheduleWithId(SqliteStatement& statement, const schedule::Schedule& schedule) {
    Status status = WithField(statement.BindInt64(1, schedule.id), "id");
    if (!status.ok()) return status;
    status = WithField(statement.BindText(2, schedule.event), "event");
    if (!status.ok()) return status;
    status = BindOptionalTime(statement, 3, schedule.start_time, "start_time");
    if (!status.ok()) return status;
    status = BindOptionalTime(statement, 4, schedule.end_time, "end_time");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, 5, schedule.location, "location");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, 6, schedule.notes, "notes");
    if (!status.ok()) return status;
    status = BindOptionalInt64(statement, 7, schedule.rule_id, "rule_id");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt(8, static_cast<int>(schedule.status)), "status");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(9, schedule.created_at.time_since_epoch().count()), "created_at");
    if (!status.ok()) return status;
    return WithField(statement.BindInt64(10, schedule.updated_at.time_since_epoch().count()), "updated_at");
}

}  // namespace voicelife::storage_sqlite::mapping
