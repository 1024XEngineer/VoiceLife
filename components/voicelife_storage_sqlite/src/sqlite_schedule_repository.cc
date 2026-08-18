#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "mapping/operation_row_mapper.h"
#include "mapping/schedule_row_mapper.h"
#include "sql/operation_sql.h"
#include "sql/schedule_sql.h"
#include "voicelife/storage_sqlite/voicelife_schema.h"

namespace voicelife::storage_sqlite {
namespace {

using schedule::DateTime;
using schedule::OperationRecord;
using schedule::QueryScheduleCommand;
using schedule::Schedule;

/** @brief 返回当前秒级系统时间。 @return 当前日程时间。 */
DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

/** @brief 创建数据库未打开的错误状态。 @return 不可用错误。 */
Status DatabaseUnavailable() { return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库尚未打开"); }

/** @brief 判断操作类型是否属于领域定义范围。 @param type 待判断类型。 @return 类型有效时返回 true。 */
bool IsValidOperationType(schedule::ScheduleOperationType type) {
    return type == schedule::ScheduleOperationType::kCreate || type == schedule::ScheduleOperationType::kUpdate ||
           type == schedule::ScheduleOperationType::kDelete;
}

/** @brief 判断实体类型是否属于领域定义范围。 @param type 待判断类型。 @return 类型有效时返回 true。 */
bool IsValidEntityType(schedule::OperationEntityType type) {
    return type == schedule::OperationEntityType::kSchedule || type == schedule::OperationEntityType::kRule ||
           type == schedule::OperationEntityType::kException;
}

/**
 * @brief 从查询语句读取一行日程。
 * @param statement 已执行的查询语句。
 * @return 日程或映射错误。
 */
Result<Schedule> ReadOneSchedule(SqliteStatement& statement) {
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return Result<Schedule>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value != SqliteStep::kRow) {
        return Result<Schedule>::Failure(ErrorCode::kNotFound, "未找到指定日程");
    }
    return mapping::ReadSchedule(statement);
}

/**
 * @brief 将可空整数绑定到 SQLite 参数。
 * @param statement 目标语句。
 * @param index 参数序号。
 * @param value 可空整数。
 * @return 绑定成功时返回成功状态。
 */
Status BindOptionalInt64(SqliteStatement& statement, int index, const std::optional<int64_t>& value) {
    return value.has_value() ? statement.BindInt64(index, *value) : statement.BindNull(index);
}

/**
 * @brief 绑定日程查询共用的筛选参数。
 * @param statement 已准备语句。
 * @param query 查询条件。
 * @param include_paging 是否绑定 limit/offset。
 * @return 绑定成功时返回成功状态。
 */
Status BindScheduleQueryFilters(SqliteStatement& statement, const QueryScheduleCommand& query, bool include_paging) {
    Status status = BindOptionalInt64(statement, 1, query.schedule_id);
    if (!status.ok()) return status;
    status = BindOptionalInt64(statement, 2, query.rule_id);
    if (!status.ok()) return status;
    if (query.status != schedule::ScheduleStatusFilter::kAll) {
        status = statement.BindInt(3, static_cast<int>(query.status));
    } else {
        status = statement.BindNull(3);
    }
    if (!status.ok()) return status;

    if (query.keyword.has_value() && !query.keyword->empty()) {
        status = statement.BindText(4, *query.keyword);
    } else {
        status = statement.BindNull(4);
    }
    if (!status.ok()) return status;

    status = query.start_from.has_value() ? statement.BindInt64(5, query.start_from->time_since_epoch().count())
                                          : statement.BindNull(5);
    if (!status.ok()) return status;
    status = query.start_to.has_value() ? statement.BindInt64(6, query.start_to->time_since_epoch().count())
                                        : statement.BindNull(6);
    if (!status.ok()) return status;
    if (!include_paging) return Status::Ok();

    status = statement.BindInt64(7, query.limit);
    if (!status.ok()) return status;
    return statement.BindInt64(8, query.offset);
}

/**
 * @brief 绑定操作查询共用的筛选参数。
 * @param statement 已准备语句。
 * @param query 查询条件。
 * @param include_paging 是否绑定 limit/offset。
 * @return 绑定成功时返回成功状态。
 */
Status BindOperationQueryFilters(SqliteStatement& statement, const schedule::QueryOperationCommand& query,
                                 bool include_paging) {
    Status status = BindOptionalInt64(statement, 1, query.operation_id);
    if (!status.ok()) return status;
    if (query.entity_type.has_value()) {
        status = statement.BindInt(2, static_cast<int>(*query.entity_type));
    } else {
        status = statement.BindNull(2);
    }
    if (!status.ok()) return status;
    status = BindOptionalInt64(statement, 3, query.entity_id);
    if (!status.ok()) return status;
    if (query.type.has_value()) {
        status = statement.BindInt(4, static_cast<int>(*query.type));
    } else {
        status = statement.BindNull(4);
    }
    if (!status.ok()) return status;
    status = query.operated_from.has_value() ? statement.BindInt64(5, query.operated_from->time_since_epoch().count())
                                             : statement.BindNull(5);
    if (!status.ok()) return status;
    status = query.operated_to.has_value() ? statement.BindInt64(6, query.operated_to->time_since_epoch().count())
                                           : statement.BindNull(6);
    if (!status.ok()) return status;
    if (query.keyword.has_value() && !query.keyword->empty()) {
        status = statement.BindText(7, *query.keyword);
    } else {
        status = statement.BindNull(7);
    }
    if (!status.ok()) return status;
    if (!include_paging) return Status::Ok();
    status = statement.BindInt64(8, query.limit);
    if (!status.ok()) return status;
    return statement.BindInt64(9, query.offset);
}

}  // namespace

SqliteScheduleRepository::SqliteScheduleRepository(SqliteDatabase& database) : database_(database) {}

Status SqliteScheduleRepository::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return DatabaseUnavailable();
    return VoiceLifeSchema::Initialize(database_);
}

Result<Schedule> SqliteScheduleRepository::Insert(const Schedule& schedule) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    return Result<Schedule>::Success(std::move(normalized));
}

Result<std::vector<Schedule>> SqliteScheduleRepository::FindAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
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

Result<Schedule> SqliteScheduleRepository::FindById(schedule::ScheduleId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return Result<Schedule>::Failure(ErrorCode::kUnavailable, DatabaseUnavailable().message);
    if (id <= 0) return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "日程标识无效");
    return FindByIdLocked(id);
}

Result<std::vector<Schedule>> SqliteScheduleRepository::Find(const QueryScheduleCommand& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) {
        return Result<std::vector<Schedule>>::Failure(ErrorCode::kUnavailable, DatabaseUnavailable().message);
    }

    const std::string sql = sql::BuildScheduleFindSql(query);
    Result<SqliteStatement> prepared = database_.Prepare(sql);
    if (!prepared.ok()) return Result<std::vector<Schedule>>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    const Status bound = BindScheduleQueryFilters(statement, query, true);
    if (!bound.ok()) return Result<std::vector<Schedule>>::Failure(bound.code, bound.message);

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

Result<int64_t> SqliteScheduleRepository::Count(const QueryScheduleCommand& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) {
        return Result<int64_t>::Failure(ErrorCode::kUnavailable, DatabaseUnavailable().message);
    }

    const std::string sql = sql::BuildScheduleCountSql(query);
    Result<SqliteStatement> prepared = database_.Prepare(sql);
    if (!prepared.ok()) return Result<int64_t>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    const Status bound = BindScheduleQueryFilters(statement, query, false);
    if (!bound.ok()) return Result<int64_t>::Failure(bound.code, bound.message);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return Result<int64_t>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value != SqliteStep::kRow) return Result<int64_t>::Failure(ErrorCode::kInternal, "统计日程未返回行");
    return Result<int64_t>::Success(statement.ColumnInt64(0));
}

Result<std::vector<Schedule>> SqliteScheduleRepository::FindOverlapping(
    DateTime start, DateTime end, std::optional<schedule::ScheduleId> exclude_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) {
        return Result<std::vector<Schedule>>::Failure(ErrorCode::kUnavailable, DatabaseUnavailable().message);
    }

    Result<SqliteStatement> prepared = database_.Prepare(sql::kFindOverlappingSchedules);
    if (!prepared.ok()) return Result<std::vector<Schedule>>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    Status status = statement.BindInt64(1, end.time_since_epoch().count());
    if (!status.ok()) return Result<std::vector<Schedule>>::Failure(status.code, status.message);
    status = statement.BindInt64(2, start.time_since_epoch().count());
    if (!status.ok()) return Result<std::vector<Schedule>>::Failure(status.code, status.message);
    status = BindOptionalInt64(statement, 3, exclude_id);
    if (!status.ok()) return Result<std::vector<Schedule>>::Failure(status.code, status.message);
    if (exclude_id.has_value()) {
        status = statement.BindInt64(4, *exclude_id);
    } else {
        status = statement.BindNull(4);
    }
    if (!status.ok()) return Result<std::vector<Schedule>>::Failure(status.code, status.message);

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

Status SqliteScheduleRepository::Update(const Schedule& schedule) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return DatabaseUnavailable();
    if (schedule.id <= 0 || schedule.event.empty())
        return Status::Error(ErrorCode::kInvalidArgument, "日程标识或名称无效");
    Result<SqliteStatement> prepared = database_.Prepare(sql::kUpdateSchedule);
    if (!prepared.ok()) return prepared.status;
    SqliteStatement statement = std::move(*prepared.value);
    Status status = mapping::BindSchedule(statement, schedule);
    if (!status.ok()) return status;
    status = statement.BindInt64(10, schedule.id);
    if (!status.ok()) return status;
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return stepped.status;
    return statement.Changes() == 1 ? Status::Ok() : Status::Error(ErrorCode::kNotFound, "日程不存在");
}

Status SqliteScheduleRepository::Delete(schedule::ScheduleId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) return DatabaseUnavailable();
    if (id <= 0) return Status::Error(ErrorCode::kInvalidArgument, "日程标识无效");

    Result<SqliteStatement> prepared = database_.Prepare(sql::kCancelSchedule);
    if (!prepared.ok()) return prepared.status;
    SqliteStatement statement = std::move(*prepared.value);
    Status status = statement.BindInt64(1, Now().time_since_epoch().count());
    if (!status.ok()) return status;
    status = statement.BindInt64(2, id);
    if (!status.ok()) return status;
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return stepped.status;
    if (statement.Changes() == 1) return Status::Ok();

    const Result<Schedule> current = FindByIdLocked(id);
    if (!current.ok()) return current.status;
    return current.value->status == schedule::ScheduleStatus::kCancelled
               ? Status::Error(ErrorCode::kConflict, "日程已取消，不能重复删除")
               : Status::Error(ErrorCode::kConflict, "日程取消未生效");
}

Result<std::vector<schedule::OperationRecord>> SqliteScheduleRepository::FindOperations(
    const schedule::QueryOperationCommand& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) {
        const Status status = DatabaseUnavailable();
        return Result<std::vector<OperationRecord>>::Failure(status.code, status.message);
    }
    Result<SqliteStatement> prepared = database_.Prepare(sql::BuildOperationFindSql(query));
    if (!prepared.ok()) {
        return Result<std::vector<OperationRecord>>::Failure(prepared.status.code, prepared.status.message);
    }
    SqliteStatement statement = std::move(*prepared.value);
    const Status bound = BindOperationQueryFilters(statement, query, true);
    if (!bound.ok()) return Result<std::vector<OperationRecord>>::Failure(bound.code, bound.message);

    std::vector<OperationRecord> operations;
    while (true) {
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok())
            return Result<std::vector<OperationRecord>>::Failure(stepped.status.code, stepped.status.message);
        if (*stepped.value == SqliteStep::kDone) break;
        const Result<OperationRecord> row = mapping::ReadOperation(statement);
        if (!row.ok()) return Result<std::vector<OperationRecord>>::Failure(row.status.code, row.status.message);
        operations.push_back(*row.value);
    }
    return Result<std::vector<OperationRecord>>::Success(std::move(operations));
}

Result<int64_t> SqliteScheduleRepository::CountOperations(const schedule::QueryOperationCommand& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) {
        const Status status = DatabaseUnavailable();
        return Result<int64_t>::Failure(status.code, status.message);
    }
    Result<SqliteStatement> prepared = database_.Prepare(sql::BuildOperationCountSql(query));
    if (!prepared.ok()) return Result<int64_t>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    const Status bound = BindOperationQueryFilters(statement, query, false);
    if (!bound.ok()) return Result<int64_t>::Failure(bound.code, bound.message);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return Result<int64_t>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value != SqliteStep::kRow) return Result<int64_t>::Failure(ErrorCode::kInternal, "统计操作未返回行");
    return Result<int64_t>::Success(statement.ColumnInt64(0));
}

Result<OperationRecord> SqliteScheduleRepository::InsertOperation(const OperationRecord& operation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_.IsOpen()) {
        const Status status = DatabaseUnavailable();
        return Result<OperationRecord>::Failure(status.code, status.message);
    }
    if (!IsValidEntityType(operation.entity_type) || !IsValidOperationType(operation.type) ||
        operation.entity_id <= 0 || operation.label.empty()) {
        return Result<OperationRecord>::Failure(ErrorCode::kInvalidArgument, "操作记录字段无效");
    }
    if ((operation.type == schedule::ScheduleOperationType::kCreate) && operation.before.has_value()) {
        return Result<OperationRecord>::Failure(ErrorCode::kInvalidArgument, "创建操作不能携带 before 快照");
    }
    if ((operation.type == schedule::ScheduleOperationType::kUpdate ||
         operation.type == schedule::ScheduleOperationType::kDelete) &&
        !operation.before.has_value()) {
        return Result<OperationRecord>::Failure(ErrorCode::kInvalidArgument, "修改和删除操作必须携带 before 快照");
    }

    OperationRecord normalized = operation;
    normalized.id = 0;
    normalized.operated_at = Now();
    Result<SqliteStatement> prepared = database_.Prepare(sql::kInsertOperation);
    if (!prepared.ok()) return Result<OperationRecord>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    const Status bound = mapping::BindOperation(statement, normalized);
    if (!bound.ok()) return Result<OperationRecord>::Failure(bound.code, bound.message);
    const Result<SqliteStep> stepped = statement.Step();
    if (!stepped.ok()) return Result<OperationRecord>::Failure(stepped.status.code, stepped.status.message);
    if (*stepped.value != SqliteStep::kDone) {
        return Result<OperationRecord>::Failure(ErrorCode::kInternal, "插入操作记录未完成");
    }
    normalized.id = statement.LastInsertRowId();
    return Result<OperationRecord>::Success(std::move(normalized));
}

Result<Schedule> SqliteScheduleRepository::FindByIdLocked(schedule::ScheduleId id) const {
    Result<SqliteStatement> prepared = database_.Prepare(sql::kFindScheduleById);
    if (!prepared.ok()) return Result<Schedule>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    Status status = statement.BindInt64(1, id);
    if (!status.ok()) return Result<Schedule>::Failure(status.code, status.message);
    return ReadOneSchedule(statement);
}

}  // namespace voicelife::storage_sqlite
