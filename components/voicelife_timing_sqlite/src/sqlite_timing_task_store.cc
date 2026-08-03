#include "voicelife/timing_sqlite/sqlite_timing_task_store.h"

#include <sqlite3.h>

#include <utility>

namespace voicelife::timing_sqlite {
namespace {
using namespace timing;

constexpr char kSchema[] = R"sql(
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS timer_task(
 id TEXT PRIMARY KEY,schedule_id TEXT NOT NULL,status INTEGER NOT NULL,next_trigger_at INTEGER NOT NULL,
 time_zone TEXT NOT NULL,recurrence_frequency INTEGER NOT NULL,recurrence_start_at INTEGER NOT NULL,
 pending_recurrence_frequency INTEGER,pending_recurrence_start_at INTEGER NOT NULL,
 pending_recurrence_time_zone TEXT NOT NULL,pending_effective_from INTEGER NOT NULL,
 effective_until INTEGER NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,deleted_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS recurrence_weekday(task_id TEXT NOT NULL REFERENCES timer_task(id),value INTEGER NOT NULL,PRIMARY KEY(task_id,value));
CREATE TABLE IF NOT EXISTS recurrence_month_day(task_id TEXT NOT NULL REFERENCES timer_task(id),value INTEGER NOT NULL,PRIMARY KEY(task_id,value));
CREATE TABLE IF NOT EXISTS recurrence_month(task_id TEXT NOT NULL REFERENCES timer_task(id),value INTEGER NOT NULL,PRIMARY KEY(task_id,value));
CREATE TABLE IF NOT EXISTS pending_recurrence_weekday(task_id TEXT NOT NULL REFERENCES timer_task(id),value INTEGER NOT NULL,PRIMARY KEY(task_id,value));
CREATE TABLE IF NOT EXISTS pending_recurrence_month_day(task_id TEXT NOT NULL REFERENCES timer_task(id),value INTEGER NOT NULL,PRIMARY KEY(task_id,value));
CREATE TABLE IF NOT EXISTS pending_recurrence_month(task_id TEXT NOT NULL REFERENCES timer_task(id),value INTEGER NOT NULL,PRIMARY KEY(task_id,value));
CREATE TABLE IF NOT EXISTS timer_instance(
 id TEXT PRIMARY KEY,task_id TEXT NOT NULL REFERENCES timer_task(id),planned_at INTEGER NOT NULL,
 planned_end_at INTEGER NOT NULL,actual_trigger_at INTEGER NOT NULL,status INTEGER NOT NULL,
 override_start_at INTEGER,override_end_at INTEGER,last_action_at INTEGER NOT NULL,
 created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,deleted_at INTEGER NOT NULL,UNIQUE(task_id,planned_at));
CREATE TABLE IF NOT EXISTS reminder_rule(
 id TEXT PRIMARY KEY,task_id TEXT NOT NULL REFERENCES timer_task(id),reminder_type INTEGER NOT NULL,
 offset_minutes INTEGER NOT NULL,max_snooze_count INTEGER NOT NULL,snooze_interval_minutes INTEGER NOT NULL,
 channel TEXT NOT NULL,source TEXT NOT NULL,status INTEGER NOT NULL,
 created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,deleted_at INTEGER NOT NULL);
CREATE UNIQUE INDEX IF NOT EXISTS one_active_strong_rule ON reminder_rule(task_id)
 WHERE reminder_type=1 AND offset_minutes=0 AND status=0 AND deleted_at=0;
CREATE TABLE IF NOT EXISTS reminder_trigger(
 id TEXT PRIMARY KEY,reminder_rule_id TEXT NOT NULL REFERENCES reminder_rule(id),
 task_id TEXT NOT NULL REFERENCES timer_task(id),instance_id TEXT NOT NULL REFERENCES timer_instance(id),
 reminder_type INTEGER NOT NULL,planned_trigger_at INTEGER NOT NULL,actual_trigger_at INTEGER NOT NULL,
 status INTEGER NOT NULL,snooze_count INTEGER NOT NULL,max_snooze_count INTEGER NOT NULL,
 delivered_at INTEGER NOT NULL,last_action_at INTEGER NOT NULL,payload TEXT NOT NULL,
 created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,deleted_at INTEGER NOT NULL,
 UNIQUE(instance_id,reminder_rule_id));
CREATE TABLE IF NOT EXISTS timing_event_outbox(
 event_id TEXT PRIMARY KEY,event_type INTEGER NOT NULL,task_id TEXT NOT NULL,instance_id TEXT NOT NULL,
 reminder_rule_id TEXT NOT NULL,reminder_trigger_id TEXT NOT NULL,schedule_id TEXT NOT NULL,
 planned_at INTEGER NOT NULL,trigger_at INTEGER NOT NULL,status INTEGER NOT NULL,occurred_at INTEGER NOT NULL,
 published INTEGER NOT NULL DEFAULT 0);
)sql";

class Statement {
   public:
    Statement(sqlite3* db, const char* sql) : db_(db) { rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr); }
    ~Statement() { sqlite3_finalize(stmt_); }
    bool ok() const { return rc_ == SQLITE_OK && stmt_ != nullptr; }
    sqlite3_stmt* get() const { return stmt_; }
    sqlite3* db() const { return db_; }
   private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
    int rc_ = SQLITE_ERROR;
};

Status DbError(sqlite3* db, const char* operation) {
    if (!db) return Status::Error(ErrorCode::kUnavailable, std::string(operation) + ": database not open");
    const int code = sqlite3_extended_errcode(db);
    ErrorCode mapped = ErrorCode::kInternal;
    if ((code & 0xff) == SQLITE_CONSTRAINT) mapped = ErrorCode::kConflict;
    const int primary = code & 0xff;
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED || primary == SQLITE_CANTOPEN) {
        mapped = ErrorCode::kUnavailable;
    }
    return Status::Error(mapped, std::string(operation) + ": " + sqlite3_errmsg(db));
}

Status RequireOpen(sqlite3* db) {
    return db ? Status::Ok() : Status::Error(ErrorCode::kUnavailable, "database not open");
}

void Text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

std::string ColumnText(sqlite3_stmt* stmt, int index) {
    const auto* value = sqlite3_column_text(stmt, index);
    return value ? reinterpret_cast<const char*>(value) : "";
}

Status Done(Statement& stmt, const char* operation) {
    if (!stmt.ok()) return DbError(stmt.db(), operation);
    return sqlite3_step(stmt.get()) == SQLITE_DONE ? Status::Ok() : DbError(stmt.db(), operation);
}

Status Exec(sqlite3* db, const char* sql, const char* operation = "execute SQL") {
    if (!db) return DbError(db, operation);
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK ? Status::Ok() : DbError(db, operation);
}

Status SaveSelector(sqlite3* db, const char* table, const std::string& task_id, const std::vector<int>& values) {
    const std::string remove = std::string("DELETE FROM ") + table + " WHERE task_id=?";
    Statement clear(db, remove.c_str());
    if (!clear.ok()) return DbError(db, "prepare selector delete");
    Text(clear.get(), 1, task_id);
    Status status = Done(clear, "delete selectors");
    const std::string insert = std::string("INSERT INTO ") + table + "(task_id,value) VALUES(?,?)";
    for (int value : values) {
        if (!status.ok()) break;
        Statement statement(db, insert.c_str());
        if (!statement.ok()) return DbError(db, "prepare selector insert");
        Text(statement.get(), 1, task_id);
        sqlite3_bind_int(statement.get(), 2, value);
        status = Done(statement, "insert selector");
    }
    return status;
}

Status SaveTask(sqlite3* db, const TimingTask& task, bool insert_only = false) {
    const char* sql = insert_only
        ? "INSERT INTO timer_task VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        : "INSERT INTO timer_task VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET schedule_id=excluded.schedule_id,status=excluded.status,next_trigger_at=excluded.next_trigger_at,time_zone=excluded.time_zone,recurrence_frequency=excluded.recurrence_frequency,recurrence_start_at=excluded.recurrence_start_at,pending_recurrence_frequency=excluded.pending_recurrence_frequency,pending_recurrence_start_at=excluded.pending_recurrence_start_at,pending_recurrence_time_zone=excluded.pending_recurrence_time_zone,pending_effective_from=excluded.pending_effective_from,effective_until=excluded.effective_until,updated_at=excluded.updated_at,deleted_at=excluded.deleted_at";
    Statement statement(db, sql);
    if (!statement.ok()) return DbError(db, "prepare task");
    Text(statement.get(), 1, task.id);
    Text(statement.get(), 2, task.schedule_id);
    sqlite3_bind_int(statement.get(), 3, static_cast<int>(task.status));
    sqlite3_bind_int64(statement.get(), 4, task.next_trigger_at);
    Text(statement.get(), 5, task.time_zone);
    sqlite3_bind_int(statement.get(), 6, static_cast<int>(task.recurrence.frequency));
    sqlite3_bind_int64(statement.get(), 7, task.recurrence.start_at);
    if (task.pending_recurrence) {
        sqlite3_bind_int(statement.get(), 8, static_cast<int>(task.pending_recurrence->frequency));
        sqlite3_bind_int64(statement.get(), 9, task.pending_recurrence->start_at);
        Text(statement.get(), 10, task.pending_recurrence->time_zone);
    } else {
        sqlite3_bind_null(statement.get(), 8);
        sqlite3_bind_int64(statement.get(), 9, 0);
        Text(statement.get(), 10, "");
    }
    sqlite3_bind_int64(statement.get(), 11, task.pending_effective_from);
    sqlite3_bind_int64(statement.get(), 12, task.effective_until);
    sqlite3_bind_int64(statement.get(), 13, task.created_at);
    sqlite3_bind_int64(statement.get(), 14, task.updated_at);
    sqlite3_bind_int64(statement.get(), 15, task.deleted_at);
    Status status = Done(statement, "save task");
    if (status.ok()) status = SaveSelector(db, "recurrence_weekday", task.id, task.recurrence.by_weekdays);
    if (status.ok()) status = SaveSelector(db, "recurrence_month_day", task.id, task.recurrence.by_month_days);
    if (status.ok()) status = SaveSelector(db, "recurrence_month", task.id, task.recurrence.by_months);
    const std::vector<int> empty;
    const RecurrenceRule* pending = task.pending_recurrence ? &*task.pending_recurrence : nullptr;
    if (status.ok()) status = SaveSelector(db, "pending_recurrence_weekday", task.id,
                                           pending ? pending->by_weekdays : empty);
    if (status.ok()) status = SaveSelector(db, "pending_recurrence_month_day", task.id,
                                           pending ? pending->by_month_days : empty);
    if (status.ok()) status = SaveSelector(db, "pending_recurrence_month", task.id,
                                           pending ? pending->by_months : empty);
    return status;
}

Result<std::vector<int>> LoadSelector(sqlite3* db, const char* table, const std::string& task_id) {
    const std::string sql = std::string("SELECT value FROM ") + table + " WHERE task_id=? ORDER BY value";
    Statement statement(db, sql.c_str());
    if (!statement.ok()) return Result<std::vector<int>>::Failure(ErrorCode::kInternal, DbError(db, "prepare selector query").message);
    Text(statement.get(), 1, task_id);
    std::vector<int> values;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) values.push_back(sqlite3_column_int(statement.get(), 0));
    if (rc != SQLITE_DONE) return Result<std::vector<int>>::Failure(ErrorCode::kInternal, DbError(db, "read selectors").message);
    return Result<std::vector<int>>::Success(std::move(values));
}

Result<TimingTask> ReadTask(sqlite3* db, sqlite3_stmt* stmt) {
    TimingTask task{.id = ColumnText(stmt, 0), .schedule_id = ColumnText(stmt, 1),
                    .next_trigger_at = sqlite3_column_int64(stmt, 3), .time_zone = ColumnText(stmt, 4),
                    .recurrence = {.frequency = static_cast<RecurrenceFrequency>(sqlite3_column_int(stmt, 5)),
                                   .start_at = sqlite3_column_int64(stmt, 6), .time_zone = ColumnText(stmt, 4)},
                    .status = static_cast<TimingTaskStatus>(sqlite3_column_int(stmt, 2)),
                    .created_at = sqlite3_column_int64(stmt, 12), .updated_at = sqlite3_column_int64(stmt, 13),
                    .effective_until = sqlite3_column_int64(stmt, 11), .deleted_at = sqlite3_column_int64(stmt, 14)};
    auto weekdays = LoadSelector(db, "recurrence_weekday", task.id);
    auto month_days = LoadSelector(db, "recurrence_month_day", task.id);
    auto months = LoadSelector(db, "recurrence_month", task.id);
    if (!weekdays.ok()) return Result<TimingTask>::Failure(weekdays.status.code, weekdays.status.message);
    if (!month_days.ok()) return Result<TimingTask>::Failure(month_days.status.code, month_days.status.message);
    if (!months.ok()) return Result<TimingTask>::Failure(months.status.code, months.status.message);
    task.recurrence.by_weekdays = std::move(*weekdays.value);
    task.recurrence.by_month_days = std::move(*month_days.value);
    task.recurrence.by_months = std::move(*months.value);
    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
        task.pending_recurrence = RecurrenceRule{
            .frequency = static_cast<RecurrenceFrequency>(sqlite3_column_int(stmt, 7)),
            .start_at = sqlite3_column_int64(stmt, 8),
            .time_zone = ColumnText(stmt, 9),
        };
        auto pending_weekdays = LoadSelector(db, "pending_recurrence_weekday", task.id);
        auto pending_month_days = LoadSelector(db, "pending_recurrence_month_day", task.id);
        auto pending_months = LoadSelector(db, "pending_recurrence_month", task.id);
        if (!pending_weekdays.ok()) return Result<TimingTask>::Failure(pending_weekdays.status.code, pending_weekdays.status.message);
        if (!pending_month_days.ok()) return Result<TimingTask>::Failure(pending_month_days.status.code, pending_month_days.status.message);
        if (!pending_months.ok()) return Result<TimingTask>::Failure(pending_months.status.code, pending_months.status.message);
        task.pending_recurrence->by_weekdays = std::move(*pending_weekdays.value);
        task.pending_recurrence->by_month_days = std::move(*pending_month_days.value);
        task.pending_recurrence->by_months = std::move(*pending_months.value);
        task.pending_effective_from = sqlite3_column_int64(stmt, 10);
    }
    return Result<TimingTask>::Success(std::move(task));
}

Status SaveInstance(sqlite3* db, const TimerInstance& value) {
    Statement statement(db, "INSERT INTO timer_instance VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET planned_end_at=excluded.planned_end_at,actual_trigger_at=excluded.actual_trigger_at,status=excluded.status,override_start_at=excluded.override_start_at,override_end_at=excluded.override_end_at,last_action_at=excluded.last_action_at,updated_at=excluded.updated_at,deleted_at=excluded.deleted_at");
    if (!statement.ok()) return DbError(db, "prepare instance");
    Text(statement.get(), 1, value.id); Text(statement.get(), 2, value.task_id);
    sqlite3_bind_int64(statement.get(), 3, value.planned_at); sqlite3_bind_int64(statement.get(), 4, value.planned_end_at);
    sqlite3_bind_int64(statement.get(), 5, value.actual_trigger_at); sqlite3_bind_int(statement.get(), 6, static_cast<int>(value.status));
    if (value.override_fields.start_at) sqlite3_bind_int64(statement.get(), 7, *value.override_fields.start_at); else sqlite3_bind_null(statement.get(), 7);
    if (value.override_fields.end_at) sqlite3_bind_int64(statement.get(), 8, *value.override_fields.end_at); else sqlite3_bind_null(statement.get(), 8);
    sqlite3_bind_int64(statement.get(), 9, value.last_action_at); sqlite3_bind_int64(statement.get(), 10, value.created_at);
    sqlite3_bind_int64(statement.get(), 11, value.updated_at); sqlite3_bind_int64(statement.get(), 12, value.deleted_at);
    return Done(statement, "save instance");
}

TimerInstance ReadInstance(sqlite3_stmt* stmt) {
    TimerInstance value{.id = ColumnText(stmt, 0), .task_id = ColumnText(stmt, 1),
                        .planned_at = sqlite3_column_int64(stmt, 2), .planned_end_at = sqlite3_column_int64(stmt, 3),
                        .actual_trigger_at = sqlite3_column_int64(stmt, 4),
                        .status = static_cast<TimerInstanceStatus>(sqlite3_column_int(stmt, 5)),
                        .last_action_at = sqlite3_column_int64(stmt, 8), .created_at = sqlite3_column_int64(stmt, 9),
                        .updated_at = sqlite3_column_int64(stmt, 10), .deleted_at = sqlite3_column_int64(stmt, 11)};
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) value.override_fields.start_at = sqlite3_column_int64(stmt, 6);
    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) value.override_fields.end_at = sqlite3_column_int64(stmt, 7);
    return value;
}

Status SaveRule(sqlite3* db, const ReminderRule& value) {
    Statement statement(db, "INSERT INTO reminder_rule VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET reminder_type=excluded.reminder_type,offset_minutes=excluded.offset_minutes,max_snooze_count=excluded.max_snooze_count,snooze_interval_minutes=excluded.snooze_interval_minutes,channel=excluded.channel,source=excluded.source,status=excluded.status,updated_at=excluded.updated_at,deleted_at=excluded.deleted_at");
    if (!statement.ok()) return DbError(db, "prepare rule");
    Text(statement.get(), 1, value.id); Text(statement.get(), 2, value.task_id);
    sqlite3_bind_int(statement.get(), 3, static_cast<int>(value.type)); sqlite3_bind_int(statement.get(), 4, value.offset_minutes);
    sqlite3_bind_int(statement.get(), 5, value.max_snooze_count); sqlite3_bind_int(statement.get(), 6, value.snooze_interval_minutes);
    Text(statement.get(), 7, value.channel); Text(statement.get(), 8, value.source);
    sqlite3_bind_int(statement.get(), 9, static_cast<int>(value.status)); sqlite3_bind_int64(statement.get(), 10, value.created_at);
    sqlite3_bind_int64(statement.get(), 11, value.updated_at); sqlite3_bind_int64(statement.get(), 12, value.deleted_at);
    return Done(statement, "save rule");
}

ReminderRule ReadRule(sqlite3_stmt* stmt) {
    return {.id = ColumnText(stmt, 0), .task_id = ColumnText(stmt, 1),
            .type = static_cast<ReminderType>(sqlite3_column_int(stmt, 2)), .offset_minutes = sqlite3_column_int(stmt, 3),
            .max_snooze_count = sqlite3_column_int(stmt, 4), .snooze_interval_minutes = sqlite3_column_int(stmt, 5),
            .channel = ColumnText(stmt, 6), .source = ColumnText(stmt, 7),
            .status = static_cast<ReminderRuleStatus>(sqlite3_column_int(stmt, 8)),
            .created_at = sqlite3_column_int64(stmt, 9), .updated_at = sqlite3_column_int64(stmt, 10),
            .deleted_at = sqlite3_column_int64(stmt, 11)};
}

Status SaveTrigger(sqlite3* db, const ReminderTrigger& value) {
    Statement statement(db, "INSERT INTO reminder_trigger VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET actual_trigger_at=excluded.actual_trigger_at,status=excluded.status,snooze_count=excluded.snooze_count,max_snooze_count=excluded.max_snooze_count,delivered_at=excluded.delivered_at,last_action_at=excluded.last_action_at,payload=excluded.payload,updated_at=excluded.updated_at,deleted_at=excluded.deleted_at");
    if (!statement.ok()) return DbError(db, "prepare trigger");
    Text(statement.get(), 1, value.id); Text(statement.get(), 2, value.reminder_rule_id); Text(statement.get(), 3, value.task_id); Text(statement.get(), 4, value.instance_id);
    sqlite3_bind_int(statement.get(), 5, static_cast<int>(value.type)); sqlite3_bind_int64(statement.get(), 6, value.planned_trigger_at);
    sqlite3_bind_int64(statement.get(), 7, value.actual_trigger_at); sqlite3_bind_int(statement.get(), 8, static_cast<int>(value.status));
    sqlite3_bind_int(statement.get(), 9, value.snooze_count); sqlite3_bind_int(statement.get(), 10, value.max_snooze_count);
    sqlite3_bind_int64(statement.get(), 11, value.delivered_at); sqlite3_bind_int64(statement.get(), 12, value.last_action_at);
    Text(statement.get(), 13, value.payload); sqlite3_bind_int64(statement.get(), 14, value.created_at);
    sqlite3_bind_int64(statement.get(), 15, value.updated_at); sqlite3_bind_int64(statement.get(), 16, value.deleted_at);
    return Done(statement, "save trigger");
}

ReminderTrigger ReadTrigger(sqlite3_stmt* stmt) {
    return {.id = ColumnText(stmt, 0), .reminder_rule_id = ColumnText(stmt, 1), .task_id = ColumnText(stmt, 2),
            .instance_id = ColumnText(stmt, 3), .type = static_cast<ReminderType>(sqlite3_column_int(stmt, 4)),
            .planned_trigger_at = sqlite3_column_int64(stmt, 5), .actual_trigger_at = sqlite3_column_int64(stmt, 6),
            .status = static_cast<ReminderTriggerStatus>(sqlite3_column_int(stmt, 7)),
            .snooze_count = sqlite3_column_int(stmt, 8), .max_snooze_count = sqlite3_column_int(stmt, 9),
            .delivered_at = sqlite3_column_int64(stmt, 10), .last_action_at = sqlite3_column_int64(stmt, 11),
            .payload = ColumnText(stmt, 12), .created_at = sqlite3_column_int64(stmt, 13),
            .updated_at = sqlite3_column_int64(stmt, 14), .deleted_at = sqlite3_column_int64(stmt, 15)};
}

Status SaveEvent(sqlite3* db, const TimingEvent& value) {
    Statement statement(db, "INSERT INTO timing_event_outbox(event_id,event_type,task_id,instance_id,reminder_rule_id,reminder_trigger_id,schedule_id,planned_at,trigger_at,status,occurred_at) VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(event_id) DO NOTHING");
    if (!statement.ok()) return DbError(db, "prepare event");
    Text(statement.get(), 1, value.event_id); sqlite3_bind_int(statement.get(), 2, static_cast<int>(value.event_type));
    Text(statement.get(), 3, value.task_id); Text(statement.get(), 4, value.instance_id); Text(statement.get(), 5, value.reminder_rule_id);
    Text(statement.get(), 6, value.reminder_trigger_id); Text(statement.get(), 7, value.schedule_id);
    sqlite3_bind_int64(statement.get(), 8, value.planned_at); sqlite3_bind_int64(statement.get(), 9, value.trigger_at);
    sqlite3_bind_int(statement.get(), 10, static_cast<int>(value.status)); sqlite3_bind_int64(statement.get(), 11, value.occurred_at);
    return Done(statement, "save event");
}

TimingEvent ReadEvent(sqlite3_stmt* stmt) {
    return {.event_type = static_cast<TimingEventType>(sqlite3_column_int(stmt, 1)), .event_id = ColumnText(stmt, 0),
            .task_id = ColumnText(stmt, 2), .instance_id = ColumnText(stmt, 3), .reminder_rule_id = ColumnText(stmt, 4),
            .reminder_trigger_id = ColumnText(stmt, 5), .schedule_id = ColumnText(stmt, 6),
            .planned_at = sqlite3_column_int64(stmt, 7), .trigger_at = sqlite3_column_int64(stmt, 8),
            .status = static_cast<TimingEventStatus>(sqlite3_column_int(stmt, 9)), .occurred_at = sqlite3_column_int64(stmt, 10)};
}

Status Begin(sqlite3* db) { return Exec(db, "BEGIN IMMEDIATE", "begin transaction"); }
Status Commit(sqlite3* db) { return Exec(db, "COMMIT", "commit transaction"); }
Status Rollback(sqlite3* db, Status failure) { Exec(db, "ROLLBACK", "rollback transaction"); return failure; }

}  // namespace

SqliteTimingTaskStore::~SqliteTimingTaskStore() { if (db_) sqlite3_close(db_); }

Status SqliteTimingTaskStore::Open() {
    if (db_) return Status::Ok();
    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        Status failure = DbError(db_, "open database");
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return failure;
    }
    const Status initialized = Exec(db_, kSchema, "initialize schema");
    if (!initialized.ok()) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    return initialized;
}

Status SqliteTimingTaskStore::RegisterTaskWithRules(const TimingTask& task, const std::vector<ReminderRule>& rules) {
    Status status = RequireOpen(db_); if (!status.ok()) return status;
    status = Begin(db_); if (status.ok()) status = SaveTask(db_, task, true);
    for (const auto& rule : rules) {
        if (!status.ok()) break;
        if (rule.task_id != task.id) {
            status = Status::Error(ErrorCode::kConflict, "rule belongs to another task");
            break;
        }
        status = SaveRule(db_, rule);
    }
    return status.ok() ? Commit(db_) : Rollback(db_, status);
}

Result<TimingTask> SqliteTimingTaskStore::FindTask(const std::string& id) {
    Status open = RequireOpen(db_); if (!open.ok()) return Result<TimingTask>::Failure(open.code, open.message);
    Statement statement(db_, "SELECT * FROM timer_task WHERE id=? AND deleted_at=0");
    if (!statement.ok()) return Result<TimingTask>::Failure(ErrorCode::kInternal, DbError(db_, "prepare task query").message);
    Text(statement.get(), 1, id);
    const int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_DONE) return Result<TimingTask>::Failure(ErrorCode::kNotFound, "task not found");
    if (rc != SQLITE_ROW) return Result<TimingTask>::Failure(ErrorCode::kInternal, DbError(db_, "read task").message);
    return ReadTask(db_, statement.get());
}

Status SqliteTimingTaskStore::UpdateTask(const TimingTask& task) {
    Status status = RequireOpen(db_); if (!status.ok()) return status;
    status = Begin(db_); if (status.ok()) status = SaveTask(db_, task);
    return status.ok() ? Commit(db_) : Rollback(db_, status);
}

Status SqliteTimingTaskStore::UpdateTaskWithEvent(const TimingTask& task, const TimingEvent& event) {
    Status status = RequireOpen(db_); if (!status.ok()) return status;
    status = Begin(db_); if (status.ok()) status = SaveTask(db_, task); if (status.ok()) status = SaveEvent(db_, event);
    return status.ok() ? Commit(db_) : Rollback(db_, status);
}

Result<std::vector<TimingTask>> SqliteTimingTaskStore::ListTasks() {
    Status open = RequireOpen(db_); if (!open.ok()) return Result<std::vector<TimingTask>>::Failure(open.code, open.message);
    Statement statement(db_, "SELECT * FROM timer_task WHERE deleted_at=0");
    if (!statement.ok()) return Result<std::vector<TimingTask>>::Failure(ErrorCode::kInternal, DbError(db_, "prepare task list").message);
    std::vector<TimingTask> values; int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) { auto value = ReadTask(db_, statement.get()); if (!value.ok()) return Result<std::vector<TimingTask>>::Failure(value.status.code, value.status.message); values.push_back(std::move(*value.value)); }
    if (rc != SQLITE_DONE) return Result<std::vector<TimingTask>>::Failure(ErrorCode::kInternal, DbError(db_, "read tasks").message);
    return Result<std::vector<TimingTask>>::Success(std::move(values));
}

Result<std::vector<TimingTask>> SqliteTimingTaskStore::ListDueTasks(int64_t now) {
    auto all = ListTasks(); if (!all.ok()) return all;
    std::vector<TimingTask> values; for (const auto& task : *all.value) if (task.status == TimingTaskStatus::kActive && task.next_trigger_at > 0 && task.next_trigger_at <= now) values.push_back(task);
    return Result<std::vector<TimingTask>>::Success(std::move(values));
}

Status SqliteTimingTaskStore::MaterializeOccurrence(const TimerInstance& instance, const std::vector<ReminderTrigger>& triggers, const TimingTask& task, const TimingEvent& event) {
    Status status = RequireOpen(db_); if (!status.ok()) return status;
    status = Begin(db_); if (status.ok()) status = SaveInstance(db_, instance);
    for (const auto& trigger : triggers) if (status.ok()) status = SaveTrigger(db_, trigger);
    if (status.ok()) status = SaveTask(db_, task);
    if (status.ok()) status = SaveEvent(db_, event);
    return status.ok() ? Commit(db_) : Rollback(db_, status);
}

Status SqliteTimingTaskStore::UpsertInstance(const TimerInstance& value) { Status open = RequireOpen(db_); return open.ok() ? SaveInstance(db_, value) : open; }

Result<std::optional<TimerInstance>> SqliteTimingTaskStore::FindInstance(const std::string& id) {
    Status open = RequireOpen(db_); if (!open.ok()) return Result<std::optional<TimerInstance>>::Failure(open.code, open.message);
    Statement statement(db_, "SELECT * FROM timer_instance WHERE id=? AND deleted_at=0"); if (!statement.ok()) return Result<std::optional<TimerInstance>>::Failure(ErrorCode::kInternal, DbError(db_, "prepare instance query").message);
    Text(statement.get(), 1, id); const int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_DONE) return Result<std::optional<TimerInstance>>::Success(std::nullopt);
    if (rc != SQLITE_ROW) return Result<std::optional<TimerInstance>>::Failure(ErrorCode::kInternal, DbError(db_, "read instance").message);
    return Result<std::optional<TimerInstance>>::Success(ReadInstance(statement.get()));
}

Result<std::optional<TimerInstance>> SqliteTimingTaskStore::FindInstanceByOccurrence(const std::string& task_id, int64_t planned_at) {
    Status open = RequireOpen(db_); if (!open.ok()) return Result<std::optional<TimerInstance>>::Failure(open.code, open.message);
    Statement statement(db_, "SELECT * FROM timer_instance WHERE task_id=? AND planned_at=? AND deleted_at=0"); if (!statement.ok()) return Result<std::optional<TimerInstance>>::Failure(ErrorCode::kInternal, DbError(db_, "prepare occurrence query").message);
    Text(statement.get(), 1, task_id); sqlite3_bind_int64(statement.get(), 2, planned_at); const int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_DONE) return Result<std::optional<TimerInstance>>::Success(std::nullopt);
    if (rc != SQLITE_ROW) return Result<std::optional<TimerInstance>>::Failure(ErrorCode::kInternal, DbError(db_, "read occurrence").message);
    return Result<std::optional<TimerInstance>>::Success(ReadInstance(statement.get()));
}

Result<std::vector<TimerInstance>> SqliteTimingTaskStore::ListInstances(const std::string& task_id) {
    Status open = RequireOpen(db_); if (!open.ok()) return Result<std::vector<TimerInstance>>::Failure(open.code, open.message);
    Statement statement(db_, "SELECT * FROM timer_instance WHERE task_id=? AND deleted_at=0"); if (!statement.ok()) return Result<std::vector<TimerInstance>>::Failure(ErrorCode::kInternal, DbError(db_, "prepare instance list").message); Text(statement.get(), 1, task_id);
    std::vector<TimerInstance> values; int rc = SQLITE_ROW; while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) values.push_back(ReadInstance(statement.get()));
    if (rc != SQLITE_DONE) return Result<std::vector<TimerInstance>>::Failure(ErrorCode::kInternal, DbError(db_, "read instances").message);
    return Result<std::vector<TimerInstance>>::Success(std::move(values));
}

Result<int> SqliteTimingTaskStore::ApplyFutureUpdate(const TimingTask& task, int64_t from, int64_t now, const TimingEvent& event) {
    Status status = RequireOpen(db_);
    if (!status.ok()) return Result<int>::Failure(status.code, status.message);
    status = Begin(db_);
    int affected = 0;
    Statement instances(db_, "UPDATE timer_instance SET deleted_at=?,updated_at=? WHERE task_id=? AND planned_at>=? AND deleted_at=0");
    if (status.ok() && !instances.ok()) status = DbError(db_, "prepare future instance update");
    if (status.ok()) {
        sqlite3_bind_int64(instances.get(), 1, now);
        sqlite3_bind_int64(instances.get(), 2, now);
        Text(instances.get(), 3, task.id);
        sqlite3_bind_int64(instances.get(), 4, from);
        status = Done(instances, "update future instances");
        affected = sqlite3_changes(db_);
    }
    Statement triggers(db_, "UPDATE reminder_trigger SET status=?,updated_at=? WHERE task_id=? AND instance_id IN(SELECT id FROM timer_instance WHERE task_id=? AND planned_at>=?) AND status IN(0,3)");
    if (status.ok() && !triggers.ok()) status = DbError(db_, "prepare future trigger update");
    if (status.ok()) {
        sqlite3_bind_int(triggers.get(), 1, static_cast<int>(ReminderTriggerStatus::kCancelled));
        sqlite3_bind_int64(triggers.get(), 2, now);
        Text(triggers.get(), 3, task.id);
        Text(triggers.get(), 4, task.id);
        sqlite3_bind_int64(triggers.get(), 5, from);
        status = Done(triggers, "cancel future triggers");
    }
    if (status.ok()) status = SaveTask(db_, task);
    if (status.ok()) status = SaveEvent(db_, event);
    if (!status.ok()) {
        const Status failure = Rollback(db_, status);
        return Result<int>::Failure(failure.code, failure.message);
    }
    status = Commit(db_);
    return status.ok() ? Result<int>::Success(affected)
                       : Result<int>::Failure(status.code, status.message);
}

Result<int> SqliteTimingTaskStore::CancelFuture(const TimingTask& task, int64_t from, int64_t now, const TimingEvent& event) {
    Status status = RequireOpen(db_);
    if (!status.ok()) return Result<int>::Failure(status.code, status.message);
    status = Begin(db_);
    int affected = 0;
    Statement instances(db_, "UPDATE timer_instance SET status=?,last_action_at=?,updated_at=? WHERE task_id=? AND planned_at>=? AND status<>?");
    if (status.ok() && !instances.ok()) status = DbError(db_, "prepare future cancellation");
    if (status.ok()) {
        sqlite3_bind_int(instances.get(), 1, static_cast<int>(TimerInstanceStatus::kSkipped));
        sqlite3_bind_int64(instances.get(), 2, now);
        sqlite3_bind_int64(instances.get(), 3, now);
        Text(instances.get(), 4, task.id);
        sqlite3_bind_int64(instances.get(), 5, from);
        sqlite3_bind_int(instances.get(), 6, static_cast<int>(TimerInstanceStatus::kSkipped));
        status = Done(instances, "cancel future instances");
        affected = sqlite3_changes(db_);
    }
    Statement triggers(db_, "UPDATE reminder_trigger SET status=?,updated_at=? WHERE task_id=? AND instance_id IN(SELECT id FROM timer_instance WHERE task_id=? AND planned_at>=?) AND status IN(0,3)");
    if (status.ok() && !triggers.ok()) status = DbError(db_, "prepare future trigger cancellation");
    if (status.ok()) {
        sqlite3_bind_int(triggers.get(), 1, static_cast<int>(ReminderTriggerStatus::kCancelled));
        sqlite3_bind_int64(triggers.get(), 2, now);
        Text(triggers.get(), 3, task.id);
        Text(triggers.get(), 4, task.id);
        sqlite3_bind_int64(triggers.get(), 5, from);
        status = Done(triggers, "cancel future triggers");
    }
    if (status.ok()) status = SaveTask(db_, task);
    if (status.ok()) status = SaveEvent(db_, event);
    if (!status.ok()) {
        const Status failure = Rollback(db_, status);
        return Result<int>::Failure(failure.code, failure.message);
    }
    status = Commit(db_);
    return status.ok() ? Result<int>::Success(affected)
                       : Result<int>::Failure(status.code, status.message);
}

Status SqliteTimingTaskStore::UpsertRules(const std::string& task_id, const std::vector<ReminderRule>& rules) {
    Status status = RequireOpen(db_);
    if (!status.ok()) return status;
    status = Begin(db_);
    for (const auto& rule : rules) {
        if (!status.ok()) break;
        if (rule.task_id != task_id) {
            status = Status::Error(ErrorCode::kConflict, "rule belongs to another task");
            break;
        }
        Statement owner(db_, "SELECT task_id FROM reminder_rule WHERE id=?");
        if (!owner.ok()) {
            status = DbError(db_, "prepare rule ownership query");
            break;
        }
        Text(owner.get(), 1, rule.id);
        const int rc = sqlite3_step(owner.get());
        if (rc == SQLITE_ROW && ColumnText(owner.get(), 0) != task_id) {
            status = Status::Error(ErrorCode::kConflict, "rule belongs to another task");
            break;
        }
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            status = DbError(db_, "read rule ownership");
            break;
        }
        status = SaveRule(db_, rule);
    }
    return status.ok() ? Commit(db_) : Rollback(db_, status);
}

Result<int> SqliteTimingTaskStore::DisableRuleAndCancelPendingTriggers(const std::string& id,int64_t now){Status status=RequireOpen(db_);if(!status.ok())return Result<int>::Failure(status.code,status.message);status=Begin(db_);Statement rule(db_,"UPDATE reminder_rule SET status=?,updated_at=? WHERE id=? AND deleted_at=0");if(status.ok()&&!rule.ok())status=DbError(db_,"prepare rule disable");if(status.ok()){sqlite3_bind_int(rule.get(),1,static_cast<int>(ReminderRuleStatus::kDisabled));sqlite3_bind_int64(rule.get(),2,now);Text(rule.get(),3,id);status=Done(rule,"disable rule");if(status.ok()&&sqlite3_changes(db_)==0)status=Status::Error(ErrorCode::kNotFound,"rule not found");}int affected=0;Statement triggers(db_,"UPDATE reminder_trigger SET status=?,updated_at=? WHERE reminder_rule_id=? AND status IN(0,3)");if(status.ok()&&!triggers.ok())status=DbError(db_,"prepare trigger cancellation");if(status.ok()){sqlite3_bind_int(triggers.get(),1,static_cast<int>(ReminderTriggerStatus::kCancelled));sqlite3_bind_int64(triggers.get(),2,now);Text(triggers.get(),3,id);status=Done(triggers,"cancel rule triggers");affected=sqlite3_changes(db_);}if(!status.ok()){const Status failure=Rollback(db_,status);return Result<int>::Failure(failure.code,failure.message);}status=Commit(db_);return status.ok()?Result<int>::Success(affected):Result<int>::Failure(status.code,status.message);}
Result<std::optional<ReminderRule>> SqliteTimingTaskStore::FindRule(const std::string&id){Status open=RequireOpen(db_);if(!open.ok())return Result<std::optional<ReminderRule>>::Failure(open.code,open.message);Statement statement(db_,"SELECT * FROM reminder_rule WHERE id=? AND deleted_at=0");if(!statement.ok())return Result<std::optional<ReminderRule>>::Failure(ErrorCode::kInternal,DbError(db_,"prepare rule query").message);Text(statement.get(),1,id);int rc=sqlite3_step(statement.get());if(rc==SQLITE_DONE)return Result<std::optional<ReminderRule>>::Success(std::nullopt);if(rc!=SQLITE_ROW)return Result<std::optional<ReminderRule>>::Failure(ErrorCode::kInternal,DbError(db_,"read rule").message);return Result<std::optional<ReminderRule>>::Success(ReadRule(statement.get()));}
Result<std::vector<ReminderRule>> SqliteTimingTaskStore::ListRules(const std::string&id){Status open=RequireOpen(db_);if(!open.ok())return Result<std::vector<ReminderRule>>::Failure(open.code,open.message);Statement statement(db_,"SELECT * FROM reminder_rule WHERE task_id=? AND deleted_at=0 ORDER BY offset_minutes");if(!statement.ok())return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal,DbError(db_,"prepare rule list").message);Text(statement.get(),1,id);std::vector<ReminderRule> values;int rc=SQLITE_ROW;while((rc=sqlite3_step(statement.get()))==SQLITE_ROW)values.push_back(ReadRule(statement.get()));if(rc!=SQLITE_DONE)return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal,DbError(db_,"read rules").message);return Result<std::vector<ReminderRule>>::Success(std::move(values));}
Status SqliteTimingTaskStore::UpsertTriggers(const std::vector<ReminderTrigger>&values){Status status=RequireOpen(db_);if(!status.ok())return status;status=Begin(db_);for(const auto&value:values)if(status.ok())status=SaveTrigger(db_,value);return status.ok()?Commit(db_):Rollback(db_,status);}
Status SqliteTimingTaskStore::UpdateTrigger(const ReminderTrigger&value){Status open=RequireOpen(db_);return open.ok()?SaveTrigger(db_,value):open;}
Status SqliteTimingTaskStore::UpdateTriggerWithEvent(const ReminderTrigger&value,const TimingEvent&event){Status status=RequireOpen(db_);if(!status.ok())return status;status=Begin(db_);if(status.ok())status=SaveTrigger(db_,value);if(status.ok())status=SaveEvent(db_,event);return status.ok()?Commit(db_):Rollback(db_,status);}
Result<std::optional<ReminderTrigger>> SqliteTimingTaskStore::FindTrigger(const std::string&id){Status open=RequireOpen(db_);if(!open.ok())return Result<std::optional<ReminderTrigger>>::Failure(open.code,open.message);Statement statement(db_,"SELECT * FROM reminder_trigger WHERE id=? AND deleted_at=0");if(!statement.ok())return Result<std::optional<ReminderTrigger>>::Failure(ErrorCode::kInternal,DbError(db_,"prepare trigger query").message);Text(statement.get(),1,id);int rc=sqlite3_step(statement.get());if(rc==SQLITE_DONE)return Result<std::optional<ReminderTrigger>>::Success(std::nullopt);if(rc!=SQLITE_ROW)return Result<std::optional<ReminderTrigger>>::Failure(ErrorCode::kInternal,DbError(db_,"read trigger").message);return Result<std::optional<ReminderTrigger>>::Success(ReadTrigger(statement.get()));}
Result<std::vector<ReminderTrigger>> SqliteTimingTaskStore::ListTriggers(){Status open=RequireOpen(db_);if(!open.ok())return Result<std::vector<ReminderTrigger>>::Failure(open.code,open.message);Statement statement(db_,"SELECT * FROM reminder_trigger WHERE deleted_at=0");if(!statement.ok())return Result<std::vector<ReminderTrigger>>::Failure(ErrorCode::kInternal,DbError(db_,"prepare trigger list").message);std::vector<ReminderTrigger> values;int rc=SQLITE_ROW;while((rc=sqlite3_step(statement.get()))==SQLITE_ROW)values.push_back(ReadTrigger(statement.get()));if(rc!=SQLITE_DONE)return Result<std::vector<ReminderTrigger>>::Failure(ErrorCode::kInternal,DbError(db_,"read triggers").message);return Result<std::vector<ReminderTrigger>>::Success(std::move(values));}
Result<std::vector<ReminderTrigger>> SqliteTimingTaskStore::ListDueTriggers(int64_t now){auto all=ListTriggers();if(!all.ok())return all;std::vector<ReminderTrigger> values;for(const auto&value:*all.value)if((value.status==ReminderTriggerStatus::kPending||value.status==ReminderTriggerStatus::kSnoozed)&&value.actual_trigger_at<=now)values.push_back(value);return Result<std::vector<ReminderTrigger>>::Success(std::move(values));}
Status SqliteTimingTaskStore::EnqueueEvent(const TimingEvent&event){Status open=RequireOpen(db_);return open.ok()?SaveEvent(db_,event):open;}
Result<std::vector<TimingEvent>> SqliteTimingTaskStore::ListPendingEvents(){Status open=RequireOpen(db_);if(!open.ok())return Result<std::vector<TimingEvent>>::Failure(open.code,open.message);Statement statement(db_,"SELECT event_id,event_type,task_id,instance_id,reminder_rule_id,reminder_trigger_id,schedule_id,planned_at,trigger_at,status,occurred_at FROM timing_event_outbox WHERE published=0 ORDER BY rowid");if(!statement.ok())return Result<std::vector<TimingEvent>>::Failure(ErrorCode::kInternal,DbError(db_,"prepare event list").message);std::vector<TimingEvent> values;int rc=SQLITE_ROW;while((rc=sqlite3_step(statement.get()))==SQLITE_ROW)values.push_back(ReadEvent(statement.get()));if(rc!=SQLITE_DONE)return Result<std::vector<TimingEvent>>::Failure(ErrorCode::kInternal,DbError(db_,"read events").message);return Result<std::vector<TimingEvent>>::Success(std::move(values));}
Status SqliteTimingTaskStore::MarkEventPublished(const std::string&id){Status open=RequireOpen(db_);if(!open.ok())return open;Statement statement(db_,"UPDATE timing_event_outbox SET published=1 WHERE event_id=?");if(!statement.ok())return DbError(db_,"prepare event publish");Text(statement.get(),1,id);Status status=Done(statement,"mark event published");if(status.ok()&&sqlite3_changes(db_)==0)return Status::Error(ErrorCode::kNotFound,"event not found");return status;}

}  // namespace voicelife::timing_sqlite
