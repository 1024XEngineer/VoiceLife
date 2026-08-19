#include "schedule_row_mapper.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace voicelife::storage_sqlite::mapping {
namespace {

/**
 * @brief 为字段错误补充字段名。
 * @param status 底层绑定状态。
 * @param field 正在绑定的字段名。
 * @return 带字段上下文的状态。
 */
Status WithField(Status status, const char* field) {
    if (status.ok()) return status;
    std::string message = std::string("绑定日程字段失败：") + field;
    if (!status.message.empty()) message += "；" + status.message;
    return Status::Error(status.code, std::move(message));
}

/**
 * @brief 绑定可空的 64 位整数。
 * @param statement SQLite 语句包装器。
 * @param index 参数序号。
 * @param value 待绑定的可选值。
 * @param field 字段名。
 * @return 绑定成功时返回成功状态。
 */
Status BindOptionalInt64(SqliteStatement& statement, int index, const std::optional<int64_t>& value,
                         const char* field) {
    return WithField(value.has_value() ? statement.BindInt64(index, *value) : statement.BindNull(index), field);
}

/**
 * @brief 绑定可空文本。
 * @param statement SQLite 语句包装器。
 * @param index 参数序号。
 * @param value 待绑定的可选文本。
 * @param field 字段名。
 * @return 绑定成功时返回成功状态。
 */
Status BindOptionalText(SqliteStatement& statement, int index, const std::optional<std::string>& value,
                        const char* field) {
    return WithField(value.has_value() ? statement.BindText(index, *value) : statement.BindNull(index), field);
}

/**
 * @brief 判断数据库状态值是否属于领域枚举。
 * @param value 数据库整数值。
 * @return 状态有效时返回 true。
 */
bool IsValidStatus(int value) {
    return value == static_cast<int>(schedule::ScheduleStatus::kActive) ||
           value == static_cast<int>(schedule::ScheduleStatus::kCancelled) ||
           value == static_cast<int>(schedule::ScheduleStatus::kCompleted);
}

/**
 * @brief 读取可空的 Unix 秒时间。
 * @param statement SQLite 语句包装器。
 * @param column 结果列序号。
 * @return 空值或转换后的日程时间。
 */
std::optional<schedule::DateTime> ReadOptionalTime(const SqliteStatement& statement, int column) {
    if (statement.IsNull(column)) return std::nullopt;
    return schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(column)}};
}

/**
 * @brief 读取可空文本。
 * @param statement SQLite 语句包装器。
 * @param column 结果列序号。
 * @return 空值或复制后的字符串。
 */
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
    status = BindOptionalInt64(statement, index++, schedule.reminder_task_id, "reminder_task_id");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt(index++, static_cast<int>(schedule.status)), "status");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(index++, schedule.created_at.time_since_epoch().count()), "created_at");
    if (!status.ok()) return status;
    return WithField(statement.BindInt64(index, schedule.updated_at.time_since_epoch().count()), "updated_at");
}

Result<schedule::Schedule> ReadSchedule(const SqliteStatement& statement) {
    const int status_value = statement.ColumnInt(8);
    if (!IsValidStatus(status_value)) {
        return Result<schedule::Schedule>::Failure(ErrorCode::kInternal, "数据库中的日程状态无效");
    }
    if (statement.IsNull(1)) {
        return Result<schedule::Schedule>::Failure(ErrorCode::kInternal, "数据库中的日程名称为空");
    }

    schedule::Schedule schedule{
        .id = statement.ColumnInt64(0),
        .event = statement.ColumnText(1),
        .start_time = ReadOptionalTime(statement, 2),
        .end_time = ReadOptionalTime(statement, 3),
        .location = ReadOptionalText(statement, 4),
        .notes = ReadOptionalText(statement, 5),
        .rule_id = std::nullopt,
        .reminder_task_id = std::nullopt,
        .status = static_cast<schedule::ScheduleStatus>(status_value),
        .created_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(9)}},
        .updated_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(10)}},
    };
    if (!statement.IsNull(6)) {
        schedule.rule_id = statement.ColumnInt64(6);
    }
    if (!statement.IsNull(7)) {
        schedule.reminder_task_id = statement.ColumnInt64(7);
    }
    return Result<schedule::Schedule>::Success(std::move(schedule));
}

}  // namespace voicelife::storage_sqlite::mapping
