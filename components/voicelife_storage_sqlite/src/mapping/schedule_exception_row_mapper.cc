#include "schedule_exception_row_mapper.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace voicelife::storage_sqlite::mapping {
namespace {

/// 为字段错误补充字段名。
Status WithField(Status status, const char* field) {
    if (status.ok()) return status;
    std::string message = std::string("绑定例外字段失败：") + field;
    if (!status.message.empty()) message += "；" + status.message;
    return Status::Error(status.code, std::move(message));
}

/// 绑定可空 64 位整数。
Status BindOptionalInt64(SqliteStatement& statement, int index, const std::optional<int64_t>& value,
                         const char* field) {
    return WithField(value.has_value() ? statement.BindInt64(index, *value) : statement.BindNull(index), field);
}

/// 绑定可空文本。
Status BindOptionalText(SqliteStatement& statement, int index, const std::optional<std::string>& value,
                        const char* field) {
    return WithField(value.has_value() ? statement.BindText(index, *value) : statement.BindNull(index), field);
}

/// 读取可空的 Unix 秒时间。
std::optional<schedule::DateTime> ReadOptionalTime(const SqliteStatement& statement, int column) {
    if (statement.IsNull(column)) return std::nullopt;
    return schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(column)}};
}

/// 读取可空文本。
std::optional<std::string> ReadOptionalText(const SqliteStatement& statement, int column) {
    if (statement.IsNull(column)) return std::nullopt;
    return statement.ColumnText(column);
}

/// 判断例外类型是否属于领域枚举。
bool IsValidExceptionType(int value) {
    return value == static_cast<int>(schedule::ExceptionType::kModify) ||
           value == static_cast<int>(schedule::ExceptionType::kSkip);
}

}  // namespace

Status BindScheduleException(SqliteStatement& statement, const schedule::ScheduleException& exception) {
    int index = 1;
    Status status = WithField(statement.BindInt64(index++, exception.rule_id), "rule_id");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(index++, exception.original_start_time.time_since_epoch().count()),
                       "original_start_time");
    if (!status.ok()) return status;
    status = BindOptionalInt64(statement, index++, exception.schedule_id, "schedule_id");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt(index++, static_cast<int>(exception.type)), "type");
    if (!status.ok()) return status;
    status = WithField(exception.override_start_time.has_value()
                           ? statement.BindInt64(index++, exception.override_start_time->time_since_epoch().count())
                           : statement.BindNull(index++),
                       "override_start_time");
    if (!status.ok()) return status;
    status = WithField(exception.override_end_time.has_value()
                           ? statement.BindInt64(index++, exception.override_end_time->time_since_epoch().count())
                           : statement.BindNull(index++),
                       "override_end_time");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, index++, exception.override_event, "override_event");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, index++, exception.override_location, "override_location");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, index++, exception.override_notes, "override_notes");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(index++, exception.created_at.time_since_epoch().count()), "created_at");
    if (!status.ok()) return status;
    return WithField(statement.BindInt64(index, exception.updated_at.time_since_epoch().count()), "updated_at");
}

Result<schedule::ScheduleException> ReadScheduleException(const SqliteStatement& statement) {
    const int type_value = statement.ColumnInt(4);
    if (!IsValidExceptionType(type_value)) {
        return Result<schedule::ScheduleException>::Failure(ErrorCode::kInternal, "数据库中的例外类型无效");
    }
    schedule::ScheduleException exception{
        .id = statement.ColumnInt64(0),
        .rule_id = statement.ColumnInt64(1),
        .original_start_time = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(2)}},
        .schedule_id = std::nullopt,
        .type = static_cast<schedule::ExceptionType>(type_value),
        .override_start_time = ReadOptionalTime(statement, 5),
        .override_end_time = ReadOptionalTime(statement, 6),
        .override_event = ReadOptionalText(statement, 7),
        .override_location = ReadOptionalText(statement, 8),
        .override_notes = ReadOptionalText(statement, 9),
        .created_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(10)}},
        .updated_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(11)}},
    };
    if (!statement.IsNull(3)) exception.schedule_id = statement.ColumnInt64(3);
    return Result<schedule::ScheduleException>::Success(std::move(exception));
}

}  // namespace voicelife::storage_sqlite::mapping
