#include "schedule_row_mapper.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace voicelife::storage_sqlite::mapping {
namespace {
Status WithField(Status status, const char* field) {
    if (status.ok()) return status;
    std::string message = std::string("绑定日程字段失败：") + field;
    if (!status.message.empty()) message += "；" + status.message;
    return Status::Error(status.code, std::move(message));
}
Status BindOptionalInt64(SqliteStatement& statement, int index, const std::optional<int64_t>& value,
                         const char* field) {
    return WithField(value.has_value() ? statement.BindInt64(index, *value) : statement.BindNull(index), field);
}
Status BindOptionalText(SqliteStatement& statement, int index, const std::optional<std::string>& value,
                        const char* field) {
    return WithField(value.has_value() ? statement.BindText(index, *value) : statement.BindNull(index), field);
}
bool IsValidStatus(int value) {
    return value == static_cast<int>(schedule::ScheduleStatus::kActive) ||
           value == static_cast<int>(schedule::ScheduleStatus::kCancelled) ||
           value == static_cast<int>(schedule::ScheduleStatus::kCompleted);
}
std::optional<schedule::DateTime> ReadOptionalTime(const SqliteStatement& statement, int column) {
    if (statement.IsNull(column)) return std::nullopt;
    return schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(column)}};
}
std::optional<std::string> ReadOptionalText(const SqliteStatement& statement, int column) {
    if (statement.IsNull(column)) return std::nullopt;
    return statement.ColumnText(column);
}
}  // namespace

Status BindSchedule(SqliteStatement& statement, const schedule::Schedule& schedule) {
    int index = 1;
    Status status = WithField(statement.BindText(index++, schedule.event), "event");
    if (!status.ok()) return status;
    status = WithField(schedule.start_time.has_value()
                           ? statement.BindInt64(index++, schedule.start_time->time_since_epoch().count())
                           : statement.BindNull(index++),
                       "start_time");
    if (!status.ok()) return status;
    status = WithField(schedule.end_time.has_value()
                           ? statement.BindInt64(index++, schedule.end_time->time_since_epoch().count())
                           : statement.BindNull(index++),
                       "end_time");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, index++, schedule.location, "location");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, index++, schedule.notes, "notes");
    if (!status.ok()) return status;
    status = BindOptionalInt64(statement, index++, schedule.rule_id, "rule_id");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt(index++, static_cast<int>(schedule.status)), "status");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(index++, schedule.created_at.time_since_epoch().count()), "created_at");
    if (!status.ok()) return status;
    return WithField(statement.BindInt64(index, schedule.updated_at.time_since_epoch().count()), "updated_at");
}

Result<schedule::Schedule> ReadSchedule(const SqliteStatement& statement) {
    const int status_value = statement.ColumnInt(7);
    if (!IsValidStatus(status_value))
        return Result<schedule::Schedule>::Failure(ErrorCode::kInternal, "数据库中的日程状态无效");
    if (statement.IsNull(1)) return Result<schedule::Schedule>::Failure(ErrorCode::kInternal, "数据库中的日程名称为空");
    schedule::Schedule schedule{
        .id = statement.ColumnInt64(0),
        .event = statement.ColumnText(1),
        .start_time = ReadOptionalTime(statement, 2),
        .end_time = ReadOptionalTime(statement, 3),
        .location = ReadOptionalText(statement, 4),
        .notes = ReadOptionalText(statement, 5),
        .rule_id = std::nullopt,
        .status = static_cast<schedule::ScheduleStatus>(status_value),
        .created_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(8)}},
        .updated_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(9)}},
    };
    if (!statement.IsNull(6)) schedule.rule_id = statement.ColumnInt64(6);
    return Result<schedule::Schedule>::Success(std::move(schedule));
}

}  // namespace voicelife::storage_sqlite::mapping
