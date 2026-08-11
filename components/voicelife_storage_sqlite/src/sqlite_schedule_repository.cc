#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"

#include <chrono>
#include <string>
#include <utility>

#include "mapping/schedule_row_mapper.h"
#include "sql/schedule_sql.h"

namespace voicelife::storage_sqlite {
namespace {

using schedule::DateTime;
using schedule::Schedule;

/**
 * @brief 返回当前秒级系统时间。
 * @return 当前日程时间。
 */
DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

/**
 * @brief 创建数据库尚未打开的错误状态。
 * @return 不可用错误。
 */
Status DatabaseUnavailable() { return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库尚未打开"); }

}  // namespace

SqliteScheduleRepository::SqliteScheduleRepository(SqliteDatabase& database) : database_(database) {}

Status SqliteScheduleRepository::Initialize() {
    if (!database_.IsOpen()) return DatabaseUnavailable();
    return database_.Execute(sql::kCreateScheduleSchema);
}

Result<Schedule> SqliteScheduleRepository::Insert(const Schedule& schedule) {
    if (!database_.IsOpen()) {
        const Status status = DatabaseUnavailable();
        return Result<Schedule>::Failure(status.code, status.message);
    }
    if (schedule.event.empty()) return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "日程名称不能为空");

    Schedule normalized = schedule;
    const DateTime now = Now();
    normalized.id = 0;
    if (normalized.created_at == DateTime{}) normalized.created_at = now;
    if (normalized.updated_at == DateTime{}) normalized.updated_at = normalized.created_at;

    {
        Result<SqliteStatement> prepared = database_.Prepare(sql::kInsertSchedule);
        if (!prepared.ok()) return Result<Schedule>::Failure(prepared.status.code, prepared.status.message);
        SqliteStatement statement = std::move(*prepared.value);
        const Status bound = mapping::BindSchedule(statement, normalized);
        if (!bound.ok()) return Result<Schedule>::Failure(bound.code, bound.message);
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok()) return Result<Schedule>::Failure(stepped.status.code, stepped.status.message);
        if (*stepped.value != SqliteStep::kDone) {
            return Result<Schedule>::Failure(ErrorCode::kInternal, "插入日程未完成");
        }
        normalized.id = statement.LastInsertRowId();
    }
    return Result<Schedule>::Success(std::move(normalized));
}

Result<std::vector<Schedule>> SqliteScheduleRepository::FindAll() const {
    if (!database_.IsOpen()) {
        const Status status = DatabaseUnavailable();
        return Result<std::vector<Schedule>>::Failure(status.code, status.message);
    }

    Result<SqliteStatement> prepared = database_.Prepare(sql::kFindAllSchedules);
    if (!prepared.ok()) return Result<std::vector<Schedule>>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    std::vector<Schedule> schedules;
    while (true) {
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok()) return Result<std::vector<Schedule>>::Failure(stepped.status.code, stepped.status.message);
        if (*stepped.value == SqliteStep::kDone) break;
        const Result<Schedule> row = mapping::ReadSchedule(statement);
        if (!row.ok()) return Result<std::vector<Schedule>>::Failure(row.status.code, row.status.message);
        schedules.push_back(*row.value);
    }
    return Result<std::vector<Schedule>>::Success(std::move(schedules));
}

}  // namespace voicelife::storage_sqlite
