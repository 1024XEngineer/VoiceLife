#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"

#include <chrono>
#include <string>
#include <utility>

#include "mapping/schedule_row_mapper.h"
#include "sql/schedule_sql.h"
#include "voicelife/storage_sqlite/voicelife_schema.h"

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
    return VoiceLifeSchema::Initialize(database_);
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

Result<std::optional<Schedule>> SqliteScheduleRepository::FindByIdempotencyKey(std::string_view key) const {
    if (!database_.IsOpen()) {
        const Status status = DatabaseUnavailable();
        return Result<std::optional<Schedule>>::Failure(status.code, status.message);
    }
    if (key.empty() || key.size() > 128) {
        return Result<std::optional<Schedule>>::Failure(ErrorCode::kInvalidArgument, "日程创建键无效");
    }

    auto prepared = database_.Prepare(sql::kFindScheduleByIdempotencyKey);
    if (!prepared.ok()) {
        return Result<std::optional<Schedule>>::Failure(prepared.status.code, prepared.status.message);
    }
    SqliteStatement statement = std::move(*prepared.value);
    const Status bound = statement.BindText(1, key);
    if (!bound.ok()) return Result<std::optional<Schedule>>::Failure(bound.code, bound.message);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return Result<std::optional<Schedule>>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value == SqliteStep::kDone) return Result<std::optional<Schedule>>::Success(std::nullopt);
    const Result<Schedule> schedule = mapping::ReadSchedule(statement);
    if (!schedule.ok()) return Result<std::optional<Schedule>>::Failure(schedule.status.code, schedule.status.message);
    const Result<SqliteStep> done = statement.Step();
    if (!done.ok()) return Result<std::optional<Schedule>>::Failure(done.status.code, done.status.message);
    if (*done.value != SqliteStep::kDone) {
        return Result<std::optional<Schedule>>::Failure(ErrorCode::kInternal, "日程创建键映射存在多个日程");
    }
    return Result<std::optional<Schedule>>::Success(std::move(*schedule.value));
}

Result<Schedule> SqliteScheduleRepository::InsertOnce(const Schedule& schedule, std::string_view key) {
    if (!database_.IsOpen()) {
        const Status status = DatabaseUnavailable();
        return Result<Schedule>::Failure(status.code, status.message);
    }
    if (key.empty() || key.size() > 128)
        return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "日程创建键无效");

    const Result<std::optional<Schedule>> existing = FindByIdempotencyKey(key);
    if (!existing.ok()) return Result<Schedule>::Failure(existing.status.code, existing.status.message);
    if (existing.value->has_value()) return Result<Schedule>::Success(**existing.value);

    const Status begin = database_.BeginTransaction();
    if (!begin.ok()) return Result<Schedule>::Failure(begin.code, begin.message);
    const auto rollback = [this](const Status& failure) {
        const Status rollback_status = database_.Rollback();
        if (!rollback_status.ok()) {
            return Result<Schedule>::Failure(rollback_status.code, "创建日程回滚失败：" + rollback_status.message);
        }
        return Result<Schedule>::Failure(failure.code, failure.message);
    };

    const Result<std::optional<Schedule>> in_transaction = FindByIdempotencyKey(key);
    if (!in_transaction.ok()) return rollback(in_transaction.status);
    if (in_transaction.value->has_value()) {
        const Status committed = database_.Commit();
        if (!committed.ok()) return rollback(committed);
        return Result<Schedule>::Success(**in_transaction.value);
    }

    const Result<Schedule> stored = Insert(schedule);
    if (!stored.ok()) return rollback(stored.status);
    auto prepared = database_.Prepare(sql::kInsertScheduleIdempotencyKey);
    if (!prepared.ok()) return rollback(prepared.status);
    SqliteStatement statement = std::move(*prepared.value);
    Status status = statement.BindText(1, key);
    if (!status.ok()) return rollback(status);
    status = statement.BindInt64(2, stored.value->id);
    if (!status.ok()) return rollback(status);
    status = statement.BindInt64(3, stored.value->created_at.time_since_epoch().count());
    if (!status.ok()) return rollback(status);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return rollback(stepped.status);
    if (*stepped.value != SqliteStep::kDone) {
        return rollback(Status::Error(ErrorCode::kInternal, "写入日程创建键未完成"));
    }
    const Status committed = database_.Commit();
    if (!committed.ok()) return rollback(committed);
    return stored;
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

Result<std::vector<schedule::DueScheduleReminder>> SqliteScheduleRepository::ClaimDueReminders(schedule::DateTime now,
                                                                                               std::size_t limit) {
    if (!database_.IsOpen()) {
        const Status status = DatabaseUnavailable();
        return Result<std::vector<schedule::DueScheduleReminder>>::Failure(status.code, status.message);
    }
    if (limit == 0) {
        return Result<std::vector<schedule::DueScheduleReminder>>::Failure(ErrorCode::kInvalidArgument,
                                                                           "领取到期提醒的数量必须大于零");
    }

    const Status begin = database_.BeginTransaction();
    if (!begin.ok()) {
        return Result<std::vector<schedule::DueScheduleReminder>>::Failure(begin.code, begin.message);
    }
    const auto rollback = [this](Status failure) {
        const Status rollback_status = database_.Rollback();
        if (!rollback_status.ok()) {
            return Result<std::vector<schedule::DueScheduleReminder>>::Failure(
                rollback_status.code, "领取到期提醒回滚失败：" + rollback_status.message);
        }
        return Result<std::vector<schedule::DueScheduleReminder>>::Failure(failure.code, failure.message);
    };

    auto due_statement = database_.Prepare(sql::kFindDueSchedules);
    if (!due_statement.ok()) return rollback(due_statement.status);
    SqliteStatement due = std::move(*due_statement.value);
    Status status = due.BindInt64(1, now.time_since_epoch().count());
    if (!status.ok()) return rollback(status);
    status = due.BindInt64(2, static_cast<int64_t>(limit));
    if (!status.ok()) return rollback(status);

    std::vector<Schedule> candidates;
    while (true) {
        const Result<SqliteStep> stepped = due.Step();
        if (!stepped.ok()) return rollback(stepped.status);
        if (*stepped.value == SqliteStep::kDone) break;
        const Result<Schedule> schedule = mapping::ReadSchedule(due);
        if (!schedule.ok()) return rollback(schedule.status);
        candidates.push_back(*schedule.value);
    }

    std::vector<schedule::DueScheduleReminder> claimed;
    claimed.reserve(candidates.size());
    for (const Schedule& candidate : candidates) {
        auto delivery_statement = database_.Prepare(sql::kInsertReminderDelivery);
        if (!delivery_statement.ok()) return rollback(delivery_statement.status);
        SqliteStatement delivery = std::move(*delivery_statement.value);
        status = delivery.BindInt64(1, candidate.id);
        if (!status.ok()) return rollback(status);
        status = delivery.BindInt64(2, now.time_since_epoch().count());
        if (!status.ok()) return rollback(status);
        const Result<SqliteStep> stepped = delivery.Step();
        if (!stepped.ok()) return rollback(stepped.status);
        if (*stepped.value != SqliteStep::kDone) {
            return rollback(Status::Error(ErrorCode::kInternal, "写入提醒投递事实未完成"));
        }
        if (delivery.Changes() == 1) {
            claimed.push_back({.schedule = candidate, .delivered_at = now});
        }
    }

    const Status committed = database_.Commit();
    if (!committed.ok()) {
        const Status rollback_status = database_.Rollback();
        if (!rollback_status.ok()) {
            return Result<std::vector<schedule::DueScheduleReminder>>::Failure(
                rollback_status.code, "领取到期提醒提交后回滚失败：" + rollback_status.message);
        }
        return Result<std::vector<schedule::DueScheduleReminder>>::Failure(committed.code, committed.message);
    }
    return Result<std::vector<schedule::DueScheduleReminder>>::Success(std::move(claimed));
}

Status SqliteScheduleRepository::Update(const Schedule& schedule) {
    if (!database_.IsOpen()) return DatabaseUnavailable();
    if (schedule.id <= 0 || schedule.event.empty())
        return Status::Error(ErrorCode::kInvalidArgument, "日程标识或名称无效");
    auto prepared = database_.Prepare(sql::kUpdateSchedule);
    if (!prepared.ok()) return prepared.status;
    SqliteStatement statement = std::move(*prepared.value);
    Status status = mapping::BindSchedule(statement, schedule);
    if (!status.ok()) return status;
    status = statement.BindInt64(10, schedule.id);
    if (!status.ok()) return status;
    auto stepped = statement.Step();
    if (!stepped.ok()) return stepped.status;
    return statement.Changes() == 1 ? Status::Ok() : Status::Error(ErrorCode::kNotFound, "日程不存在");
}

Status SqliteScheduleRepository::Delete(schedule::ScheduleId id) {
    if (!database_.IsOpen()) return DatabaseUnavailable();
    if (id <= 0) return Status::Error(ErrorCode::kInvalidArgument, "日程标识无效");
    auto prepared = database_.Prepare(sql::kDeleteSchedule);
    if (!prepared.ok()) return prepared.status;
    SqliteStatement statement = std::move(*prepared.value);
    Status status = statement.BindInt64(1, id);
    if (!status.ok()) return status;
    auto stepped = statement.Step();
    if (!stepped.ok()) return stepped.status;
    return statement.Changes() == 1 ? Status::Ok() : Status::Error(ErrorCode::kNotFound, "日程不存在");
}

}  // namespace voicelife::storage_sqlite
