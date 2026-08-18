#include "schedule_rule_row_mapper.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "voicelife/schedule/calendar.h"

namespace voicelife::storage_sqlite::mapping {
namespace {

/// 为字段错误补充字段名。
Status WithField(Status status, const char* field) {
    if (status.ok()) return status;
    std::string message = std::string("绑定规则字段失败：") + field;
    if (!status.message.empty()) message += "；" + status.message;
    return Status::Error(status.code, std::move(message));
}

/// 绑定可空整数。
Status BindOptionalInt(SqliteStatement& statement, int index, const std::optional<int>& value, const char* field) {
    return WithField(value.has_value() ? statement.BindInt(index, *value) : statement.BindNull(index), field);
}

/// 绑定可空文本。
Status BindOptionalText(SqliteStatement& statement, int index, const std::optional<std::string>& value,
                        const char* field) {
    return WithField(value.has_value() ? statement.BindText(index, *value) : statement.BindNull(index), field);
}

/// 读取可空整数。
std::optional<int> ReadOptionalInt(const SqliteStatement& statement, int column) {
    if (statement.IsNull(column)) return std::nullopt;
    return statement.ColumnInt(column);
}

/// 读取可空文本。
std::optional<std::string> ReadOptionalText(const SqliteStatement& statement, int column) {
    if (statement.IsNull(column)) return std::nullopt;
    return statement.ColumnText(column);
}

/// 本地时刻 → 当日 0 点起的秒数。
int64_t LocalTimeToSeconds(const schedule::LocalTime& value) {
    return value.hour * 3600 + value.minute * 60 + value.second;
}

/// 当日 0 点起的秒数 → 本地时刻。
schedule::LocalTime SecondsToLocalTime(int64_t seconds) {
    return schedule::LocalTime{static_cast<int>(seconds / 3600), static_cast<int>((seconds % 3600) / 60),
                               static_cast<int>(seconds % 60)};
}

/// 本地日期 → 自 1970-01-01 起的天数。
int64_t LocalDateToDays(const schedule::LocalDate& value) {
    return schedule::DaysFromCivil(value.year, value.month, value.day);
}

/// 自 1970-01-01 起的天数 → 本地日期。
schedule::LocalDate DaysToLocalDate(int64_t days) {
    schedule::LocalDate value;
    schedule::CivilFromDays(days, value.year, value.month, value.day);
    return value;
}

/// 判断频率是否属于领域枚举。
bool IsValidFrequency(int value) { return value >= 1 && value <= 4; }

/// 判断月模式是否属于领域枚举。
bool IsValidMonthlyMode(int value) { return value == 1 || value == 2; }

/// 判断状态是否属于领域枚举。
bool IsValidStatus(int value) {
    return value == static_cast<int>(schedule::ScheduleStatus::kActive) ||
           value == static_cast<int>(schedule::ScheduleStatus::kCancelled) ||
           value == static_cast<int>(schedule::ScheduleStatus::kCompleted);
}

}  // namespace

Status BindScheduleRule(SqliteStatement& statement, const schedule::ScheduleRule& rule) {
    int index = 1;
    Status status = WithField(statement.BindText(index++, rule.event), "event");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, index++, rule.location, "location");
    if (!status.ok()) return status;
    status = BindOptionalText(statement, index++, rule.notes, "notes");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt(index++, static_cast<int>(rule.freq_type)), "freq_type");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt(index++, rule.interval_val), "interval_val");
    if (!status.ok()) return status;
    status = BindOptionalInt(statement, index++, rule.weekdays_mask, "weekdays_mask");
    if (!status.ok()) return status;
    status = BindOptionalInt(statement, index++, rule.day_of_month, "day_of_month");
    if (!status.ok()) return status;
    status = BindOptionalInt(statement, index++, rule.month_of_year, "month_of_year");
    if (!status.ok()) return status;
    status = BindOptionalInt(
        statement, index++,
        rule.monthly_mode.has_value() ? std::optional<int>{static_cast<int>(*rule.monthly_mode)} : std::nullopt,
        "monthly_mode");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt(index++, static_cast<int>(LocalTimeToSeconds(rule.start_time))), "start_time");
    if (!status.ok()) return status;
    status = BindOptionalInt(statement, index++,
                             rule.end_time.has_value()
                                 ? std::optional<int>{static_cast<int>(LocalTimeToSeconds(*rule.end_time))}
                                 : std::nullopt,
                             "end_time");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(index++, LocalDateToDays(rule.start_date)), "start_date");
    if (!status.ok()) return status;
    status = BindOptionalInt(statement, index++,
                             rule.end_date.has_value()
                                 ? std::optional<int>{static_cast<int>(LocalDateToDays(*rule.end_date))}
                                 : std::nullopt,
                             "end_date");
    if (!status.ok()) return status;
    status = BindOptionalInt(statement, index++, rule.occurrence_count, "occurrence_count");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt(index++, static_cast<int>(rule.status)), "status");
    if (!status.ok()) return status;
    status = WithField(statement.BindInt64(index++, rule.created_at.time_since_epoch().count()), "created_at");
    if (!status.ok()) return status;
    return WithField(statement.BindInt64(index, rule.updated_at.time_since_epoch().count()), "updated_at");
}

Result<schedule::ScheduleRule> ReadScheduleRule(const SqliteStatement& statement) {
    const int freq_value = statement.ColumnInt(4);
    if (!IsValidFrequency(freq_value)) {
        return Result<schedule::ScheduleRule>::Failure(ErrorCode::kInternal, "数据库中的规则频率无效");
    }
    const int status_value = statement.ColumnInt(15);
    if (!IsValidStatus(status_value)) {
        return Result<schedule::ScheduleRule>::Failure(ErrorCode::kInternal, "数据库中的规则状态无效");
    }
    if (statement.IsNull(1)) {
        return Result<schedule::ScheduleRule>::Failure(ErrorCode::kInternal, "数据库中的规则名称为空");
    }
    const std::optional<int> monthly_mode = ReadOptionalInt(statement, 9);
    if (monthly_mode.has_value() && !IsValidMonthlyMode(*monthly_mode)) {
        return Result<schedule::ScheduleRule>::Failure(ErrorCode::kInternal, "数据库中的月模式无效");
    }

    schedule::ScheduleRule rule{
        .id = statement.ColumnInt64(0),
        .event = statement.ColumnText(1),
        .location = ReadOptionalText(statement, 2),
        .notes = ReadOptionalText(statement, 3),
        .freq_type = static_cast<schedule::Frequency>(freq_value),
        .interval_val = statement.ColumnInt(5),
        .weekdays_mask = std::nullopt,
        .day_of_month = std::nullopt,
        .month_of_year = std::nullopt,
        .monthly_mode = std::nullopt,
        .start_time = SecondsToLocalTime(statement.ColumnInt(10)),
        .end_time = std::nullopt,
        .start_date = DaysToLocalDate(statement.ColumnInt64(12)),
        .end_date = std::nullopt,
        .occurrence_count = std::nullopt,
        .status = static_cast<schedule::ScheduleStatus>(status_value),
        .created_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(16)}},
        .updated_at = schedule::DateTime{std::chrono::seconds{statement.ColumnInt64(17)}},
    };
    const std::optional<int> weekdays_mask = ReadOptionalInt(statement, 6);
    if (weekdays_mask.has_value()) rule.weekdays_mask = static_cast<uint8_t>(*weekdays_mask);
    const std::optional<int> day_of_month = ReadOptionalInt(statement, 7);
    if (day_of_month.has_value()) rule.day_of_month = static_cast<uint8_t>(*day_of_month);
    const std::optional<int> month_of_year = ReadOptionalInt(statement, 8);
    if (month_of_year.has_value()) rule.month_of_year = static_cast<uint8_t>(*month_of_year);
    if (monthly_mode.has_value()) rule.monthly_mode = static_cast<schedule::MonthlyMode>(*monthly_mode);
    if (!statement.IsNull(11)) rule.end_time = SecondsToLocalTime(statement.ColumnInt(11));
    if (!statement.IsNull(13)) rule.end_date = DaysToLocalDate(statement.ColumnInt64(13));
    const std::optional<int> occurrence_count = ReadOptionalInt(statement, 14);
    if (occurrence_count.has_value()) rule.occurrence_count = *occurrence_count;
    return Result<schedule::ScheduleRule>::Success(std::move(rule));
}

}  // namespace voicelife::storage_sqlite::mapping
