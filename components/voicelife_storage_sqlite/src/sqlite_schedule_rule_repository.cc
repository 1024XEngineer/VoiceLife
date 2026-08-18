#include "voicelife/storage_sqlite/sqlite_schedule_rule_repository.h"

#include <chrono>
#include <utility>

#include "mapping/schedule_exception_row_mapper.h"
#include "mapping/schedule_row_mapper.h"
#include "mapping/schedule_rule_row_mapper.h"
#include "sql/schedule_exception_sql.h"
#include "sql/schedule_rule_sql.h"
#include "sql/schedule_sql.h"
#include "voicelife/storage_sqlite/voicelife_schema.h"

namespace voicelife::storage_sqlite {
namespace {

using schedule::DateTime;
using schedule::Schedule;
using schedule::ScheduleException;
using schedule::ScheduleRule;

/** @brief 返回当前秒级系统时间。 */
DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

/** @brief 创建数据库未打开的错误状态。 */
Status DatabaseUnavailable() { return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库尚未打开"); }

/** @brief 将回滚失败信息附加到原始事务错误。 */
Status CombineRollbackFailure(const Status& failure, const Status& rollback) {
    if (rollback.ok()) return failure;
    return Status::Error(failure.code, failure.message + "；事务回滚失败：" + rollback.message);
}

/** @brief 将失败转为回滚后的错误状态。 */
Status RollbackAfterFailure(SqliteDatabase& database, const Status& failure) {
    return CombineRollbackFailure(failure, database.Rollback());
}

}  // namespace

SqliteScheduleRuleRepository::SqliteScheduleRuleRepository(SqliteDatabase& database) : database_(database) {}

Status SqliteScheduleRuleRepository::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return DatabaseUnavailable();
    return VoiceLifeSchema::Initialize(database_);
}

Result<ScheduleRule> SqliteScheduleRuleRepository::InsertRuleLocked(const ScheduleRule& rule) {
    ScheduleRule normalized = rule;
    const DateTime now = Now();
    normalized.id = 0;
    if (normalized.created_at == DateTime{}) normalized.created_at = now;
    if (normalized.updated_at == DateTime{}) normalized.updated_at = normalized.created_at;

    Result<SqliteStatement> prepared = database_.Prepare(sql::kInsertScheduleRule);
    if (!prepared.ok()) return Result<ScheduleRule>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    const Status bound = mapping::BindScheduleRule(statement, normalized);
    if (!bound.ok()) return Result<ScheduleRule>::Failure(bound.code, bound.message);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return Result<ScheduleRule>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value != SqliteStep::kDone)
        return Result<ScheduleRule>::Failure(ErrorCode::kInternal, "插入规则未完成");
    normalized.id = statement.LastInsertRowId();
    return Result<ScheduleRule>::Success(std::move(normalized));
}

Result<Schedule> SqliteScheduleRuleRepository::InsertScheduleLocked(const Schedule& schedule) {
    Schedule normalized = schedule;
    const DateTime now = Now();
    normalized.id = 0;
    if (normalized.created_at == DateTime{}) normalized.created_at = now;
    if (normalized.updated_at == DateTime{}) normalized.updated_at = normalized.created_at;

    Result<SqliteStatement> prepared = database_.Prepare(sql::kInsertSchedule);
    if (!prepared.ok()) return Result<Schedule>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    const Status bound = mapping::BindSchedule(statement, normalized);
    if (!bound.ok()) return Result<Schedule>::Failure(bound.code, bound.message);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return Result<Schedule>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value != SqliteStep::kDone) return Result<Schedule>::Failure(ErrorCode::kInternal, "插入实例未完成");
    normalized.id = statement.LastInsertRowId();
    return Result<Schedule>::Success(std::move(normalized));
}

Result<ScheduleRule> SqliteScheduleRuleRepository::Insert(const ScheduleRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen())
        return Result<ScheduleRule>::Failure(ErrorCode::kUnavailable, DatabaseUnavailable().message);
    if (rule.event.empty()) return Result<ScheduleRule>::Failure(ErrorCode::kInvalidArgument, "规则名称不能为空");
    return InsertRuleLocked(rule);
}

Status SqliteScheduleRuleRepository::Update(const ScheduleRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return DatabaseUnavailable();
    if (rule.id <= 0 || rule.event.empty()) return Status::Error(ErrorCode::kInvalidArgument, "规则标识或名称无效");

    Result<SqliteStatement> prepared = database_.Prepare(sql::kUpdateScheduleRule);
    if (!prepared.ok()) return prepared.status;
    SqliteStatement statement = std::move(*prepared.value);
    Status status = mapping::BindScheduleRule(statement, rule);
    if (!status.ok()) return status;
    status = statement.BindInt64(18, rule.id);
    if (!status.ok()) return status;
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return stepped.status;
    return statement.Changes() == 1 ? Status::Ok() : Status::Error(ErrorCode::kNotFound, "规则不存在");
}

Result<std::vector<ScheduleRule>> SqliteScheduleRuleRepository::FindAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen())
        return Result<std::vector<ScheduleRule>>::Failure(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    Result<SqliteStatement> prepared = database_.Prepare(sql::kFindAllScheduleRules);
    if (!prepared.ok())
        return Result<std::vector<ScheduleRule>>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    std::vector<ScheduleRule> rules;
    while (true) {
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok())
            return Result<std::vector<ScheduleRule>>::Failure(stepped.status.code, stepped.status.message);
        if (*stepped.value == SqliteStep::kDone) break;
        const Result<ScheduleRule> row = mapping::ReadScheduleRule(statement);
        if (!row.ok()) return Result<std::vector<ScheduleRule>>::Failure(row.status.code, row.status.message);
        rules.push_back(*row.value);
    }
    return Result<std::vector<ScheduleRule>>::Success(std::move(rules));
}

Result<ScheduleRule> SqliteScheduleRuleRepository::FindById(schedule::ScheduleRuleId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return Result<ScheduleRule>::Failure(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    Result<SqliteStatement> prepared = database_.Prepare(sql::kFindScheduleRuleById);
    if (!prepared.ok()) return Result<ScheduleRule>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    Status status = statement.BindInt64(1, id);
    if (!status.ok()) return Result<ScheduleRule>::Failure(status.code, status.message);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return Result<ScheduleRule>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value != SqliteStep::kRow) return Result<ScheduleRule>::Failure(ErrorCode::kNotFound, "规则不存在");
    return mapping::ReadScheduleRule(statement);
}

Result<ScheduleRule> SqliteScheduleRuleRepository::CreateWithFirstInstance(
    const ScheduleRule& rule, const std::optional<Schedule>& first_instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return Result<ScheduleRule>::Failure(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    if (rule.event.empty()) return Result<ScheduleRule>::Failure(ErrorCode::kInvalidArgument, "规则名称不能为空");

    const Status begin = database_.BeginTransaction();
    if (!begin.ok()) return Result<ScheduleRule>::Failure(begin.code, begin.message);

    const Result<ScheduleRule> inserted_rule = InsertRuleLocked(rule);
    if (!inserted_rule.ok()) {
        const Status failure = RollbackAfterFailure(database_, inserted_rule.status);
        return Result<ScheduleRule>::Failure(failure.code, failure.message);
    }

    if (first_instance.has_value()) {
        Schedule schedule = *first_instance;
        schedule.rule_id = inserted_rule.value->id;
        const Result<Schedule> inserted = InsertScheduleLocked(schedule);
        if (!inserted.ok()) {
            const Status failure = RollbackAfterFailure(database_, inserted.status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
    }

    const Status committed = database_.Commit();
    if (!committed.ok()) {
        const Status failure = CombineRollbackFailure(committed, database_.Rollback());
        return Result<ScheduleRule>::Failure(failure.code, failure.message);
    }
    return Result<ScheduleRule>::Success(*inserted_rule.value);
}

Result<ScheduleRule> SqliteScheduleRuleRepository::UpdateAndRebuild(const ScheduleRule& rule,
                                                                    const std::optional<Schedule>& first_instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return Result<ScheduleRule>::Failure(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    if (rule.id <= 0 || rule.event.empty())
        return Result<ScheduleRule>::Failure(ErrorCode::kInvalidArgument, "规则标识或名称无效");

    const Status begin = database_.BeginTransaction();
    if (!begin.ok()) return Result<ScheduleRule>::Failure(begin.code, begin.message);

    {
        Result<SqliteStatement> prepared = database_.Prepare(sql::kUpdateScheduleRule);
        if (!prepared.ok()) {
            const Status failure = RollbackAfterFailure(database_, prepared.status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        SqliteStatement statement = std::move(*prepared.value);
        Status status = mapping::BindScheduleRule(statement, rule);
        if (!status.ok()) {
            const Status failure = RollbackAfterFailure(database_, status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        status = statement.BindInt64(18, rule.id);
        if (!status.ok()) {
            const Status failure = RollbackAfterFailure(database_, status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok()) {
            const Status failure = RollbackAfterFailure(database_, stepped.status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        if (statement.Changes() != 1) {
            const Status failure = RollbackAfterFailure(database_, Status::Error(ErrorCode::kNotFound, "规则不存在"));
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
    }

    const int64_t now = Now().time_since_epoch().count();
    {
        Result<SqliteStatement> prepared = database_.Prepare(sql::kDeleteFutureSchedulesByRule);
        if (!prepared.ok()) {
            const Status failure = RollbackAfterFailure(database_, prepared.status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        SqliteStatement statement = std::move(*prepared.value);
        Status status = statement.BindInt64(1, rule.id);
        if (!status.ok()) {
            const Status failure = RollbackAfterFailure(database_, status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        status = statement.BindInt64(2, now);
        if (!status.ok()) {
            const Status failure = RollbackAfterFailure(database_, status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok()) {
            const Status failure = RollbackAfterFailure(database_, stepped.status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
    }
    {
        Result<SqliteStatement> prepared = database_.Prepare(sql::kDeleteFutureExceptionsByRule);
        if (!prepared.ok()) {
            const Status failure = RollbackAfterFailure(database_, prepared.status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        SqliteStatement statement = std::move(*prepared.value);
        Status status = statement.BindInt64(1, rule.id);
        if (!status.ok()) {
            const Status failure = RollbackAfterFailure(database_, status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        status = statement.BindInt64(2, now);
        if (!status.ok()) {
            const Status failure = RollbackAfterFailure(database_, status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok()) {
            const Status failure = RollbackAfterFailure(database_, stepped.status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
    }

    if (first_instance.has_value()) {
        Schedule schedule = *first_instance;
        schedule.rule_id = rule.id;
        const Result<Schedule> inserted = InsertScheduleLocked(schedule);
        if (!inserted.ok()) {
            const Status failure = RollbackAfterFailure(database_, inserted.status);
            return Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
    }

    const Status committed = database_.Commit();
    if (!committed.ok()) {
        const Status failure = CombineRollbackFailure(committed, database_.Rollback());
        return Result<ScheduleRule>::Failure(failure.code, failure.message);
    }
    return Result<ScheduleRule>::Success(rule);
}

Status SqliteScheduleRuleRepository::CancelRuleAndInstances(schedule::ScheduleRuleId id,
                                                            int64_t& cancelled_instance_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return DatabaseUnavailable();
    if (id <= 0) return Status::Error(ErrorCode::kInvalidArgument, "规则标识无效");

    const Status begin = database_.BeginTransaction();
    if (!begin.ok()) return begin;

    const DateTime now = Now();
    {
        Result<SqliteStatement> prepared = database_.Prepare(sql::kCancelScheduleRuleById);
        if (!prepared.ok()) return RollbackAfterFailure(database_, prepared.status);
        SqliteStatement statement = std::move(*prepared.value);
        Status status = statement.BindInt64(1, now.time_since_epoch().count());
        if (!status.ok()) return RollbackAfterFailure(database_, status);
        status = statement.BindInt64(2, id);
        if (!status.ok()) return RollbackAfterFailure(database_, status);
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok()) return RollbackAfterFailure(database_, stepped.status);
        if (statement.Changes() != 1) {
            return RollbackAfterFailure(database_, Status::Error(ErrorCode::kNotFound, "规则不存在"));
        }
    }

    cancelled_instance_count = 0;
    {
        Result<SqliteStatement> prepared = database_.Prepare(sql::kCancelSchedulesByRule);
        if (!prepared.ok()) return RollbackAfterFailure(database_, prepared.status);
        SqliteStatement statement = std::move(*prepared.value);
        Status status = statement.BindInt64(1, now.time_since_epoch().count());
        if (!status.ok()) return RollbackAfterFailure(database_, status);
        status = statement.BindInt64(2, id);
        if (!status.ok()) return RollbackAfterFailure(database_, status);
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok()) return RollbackAfterFailure(database_, stepped.status);
        cancelled_instance_count = statement.Changes();
    }

    {
        Result<SqliteStatement> prepared = database_.Prepare(sql::kDeleteExceptionsByRule);
        if (!prepared.ok()) return RollbackAfterFailure(database_, prepared.status);
        SqliteStatement statement = std::move(*prepared.value);
        Status status = statement.BindInt64(1, id);
        if (!status.ok()) return RollbackAfterFailure(database_, status);
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok()) return RollbackAfterFailure(database_, stepped.status);
    }

    const Status committed = database_.Commit();
    if (!committed.ok()) return CombineRollbackFailure(committed, database_.Rollback());
    return Status::Ok();
}

Result<Schedule> SqliteScheduleRuleRepository::CreateNextInstance(
    const Schedule& schedule, const std::optional<ScheduleException>& linked_exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return Result<Schedule>::Failure(ErrorCode::kUnavailable, DatabaseUnavailable().message);
    if (schedule.event.empty() || schedule.rule_id <= 0) {
        return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "日程实例字段无效");
    }

    const Status begin = database_.BeginTransaction();
    if (!begin.ok()) return Result<Schedule>::Failure(begin.code, begin.message);

    const Result<Schedule> inserted = InsertScheduleLocked(schedule);
    if (!inserted.ok()) {
        const Status failure = RollbackAfterFailure(database_, inserted.status);
        return Result<Schedule>::Failure(failure.code, failure.message);
    }

    if (linked_exception.has_value()) {
        ScheduleException linked = *linked_exception;
        linked.schedule_id = inserted.value->id;
        const Result<ScheduleException> saved = UpsertExceptionLocked(linked);
        if (!saved.ok()) {
            const Status failure = RollbackAfterFailure(database_, saved.status);
            return Result<Schedule>::Failure(failure.code, failure.message);
        }
    }

    const Status committed = database_.Commit();
    if (!committed.ok()) {
        const Status failure = CombineRollbackFailure(committed, database_.Rollback());
        return Result<Schedule>::Failure(failure.code, failure.message);
    }
    return Result<Schedule>::Success(*inserted.value);
}

Result<std::optional<ScheduleException>> SqliteScheduleRuleRepository::FindByRuleAndTimeLocked(
    schedule::ScheduleRuleId rule_id, DateTime original_start_time) const {
    Result<SqliteStatement> prepared = database_.Prepare(sql::kFindExceptionByRuleAndTime);
    if (!prepared.ok()) {
        return Result<std::optional<ScheduleException>>::Failure(prepared.status.code, prepared.status.message);
    }
    SqliteStatement statement = std::move(*prepared.value);
    Status status = statement.BindInt64(1, rule_id);
    if (!status.ok()) return Result<std::optional<ScheduleException>>::Failure(status.code, status.message);
    status = statement.BindInt64(2, original_start_time.time_since_epoch().count());
    if (!status.ok()) return Result<std::optional<ScheduleException>>::Failure(status.code, status.message);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok())
        return Result<std::optional<ScheduleException>>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value != SqliteStep::kRow) return Result<std::optional<ScheduleException>>::Success(std::nullopt);
    const Result<ScheduleException> row = mapping::ReadScheduleException(statement);
    if (!row.ok()) return Result<std::optional<ScheduleException>>::Failure(row.status.code, row.status.message);
    return Result<std::optional<ScheduleException>>::Success(*row.value);
}

Result<ScheduleException> SqliteScheduleRuleRepository::UpsertExceptionLocked(const ScheduleException& exception) {
    if (!database_.IsOpen())
        return Result<ScheduleException>::Failure(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    if (exception.rule_id <= 0)
        return Result<ScheduleException>::Failure(ErrorCode::kInvalidArgument, "例外规则标识无效");

    ScheduleException normalized = exception;
    const DateTime now = Now();
    normalized.id = 0;
    if (normalized.created_at == DateTime{}) normalized.created_at = now;
    if (normalized.updated_at == DateTime{}) normalized.updated_at = now;

    Result<SqliteStatement> prepared = database_.Prepare(sql::kUpsertScheduleException);
    if (!prepared.ok()) return Result<ScheduleException>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    const Status bound = mapping::BindScheduleException(statement, normalized);
    if (!bound.ok()) return Result<ScheduleException>::Failure(bound.code, bound.message);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return Result<ScheduleException>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value != SqliteStep::kDone)
        return Result<ScheduleException>::Failure(ErrorCode::kInternal, "写入例外未完成");

    const Result<std::optional<ScheduleException>> found =
        FindByRuleAndTimeLocked(exception.rule_id, exception.original_start_time);
    if (!found.ok()) return Result<ScheduleException>::Failure(found.status.code, found.status.message);
    if (!found.value->has_value()) return Result<ScheduleException>::Failure(ErrorCode::kInternal, "写入例外后未找到");
    return Result<ScheduleException>::Success(**found.value);
}

Result<ScheduleException> SqliteScheduleRuleRepository::Upsert(const ScheduleException& exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    return UpsertExceptionLocked(exception);
}

Result<std::vector<ScheduleException>> SqliteScheduleRuleRepository::FindByRule(
    schedule::ScheduleRuleId rule_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) {
        return Result<std::vector<ScheduleException>>::Failure(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    }
    Result<SqliteStatement> prepared = database_.Prepare(sql::kFindExceptionsByRule);
    if (!prepared.ok()) {
        return Result<std::vector<ScheduleException>>::Failure(prepared.status.code, prepared.status.message);
    }
    SqliteStatement statement = std::move(*prepared.value);
    Status status = statement.BindInt64(1, rule_id);
    if (!status.ok()) return Result<std::vector<ScheduleException>>::Failure(status.code, status.message);
    std::vector<ScheduleException> exceptions;
    while (true) {
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok())
            return Result<std::vector<ScheduleException>>::Failure(stepped.status.code, stepped.status.message);
        if (*stepped.value == SqliteStep::kDone) break;
        const Result<ScheduleException> row = mapping::ReadScheduleException(statement);
        if (!row.ok()) return Result<std::vector<ScheduleException>>::Failure(row.status.code, row.status.message);
        exceptions.push_back(*row.value);
    }
    return Result<std::vector<ScheduleException>>::Success(std::move(exceptions));
}

Result<std::optional<ScheduleException>> SqliteScheduleRuleRepository::FindByRuleAndTime(
    schedule::ScheduleRuleId rule_id, DateTime original_start_time) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) {
        return Result<std::optional<ScheduleException>>::Failure(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    }
    return FindByRuleAndTimeLocked(rule_id, original_start_time);
}

Status SqliteScheduleRuleRepository::DeleteFuture(schedule::ScheduleRuleId rule_id, DateTime after) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return DatabaseUnavailable();
    Result<SqliteStatement> prepared = database_.Prepare(sql::kDeleteFutureExceptionsByRule);
    if (!prepared.ok()) return prepared.status;
    SqliteStatement statement = std::move(*prepared.value);
    Status status = statement.BindInt64(1, rule_id);
    if (!status.ok()) return status;
    status = statement.BindInt64(2, after.time_since_epoch().count());
    if (!status.ok()) return status;
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return stepped.status;
    return Status::Ok();
}

}  // namespace voicelife::storage_sqlite
