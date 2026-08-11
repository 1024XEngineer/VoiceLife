#include "voicelife/storage_sqlite/sqlite_database.h"

#include <limits>
#include <string>
#include <utility>

#include "sqlite3.h"

namespace voicelife::storage_sqlite {
namespace {

/**
 * @brief 将 SQLite 连接错误转换为项目状态。
 * @param database SQLite 连接，可为空。
 * @param result SQLite 返回码。
 * @param operation 失败操作说明。
 * @return 映射后的错误状态。
 */
Status MakeSqliteFailure(sqlite3* database, int result, const char* operation) {
    const int primary = result & 0xff;
    ErrorCode code = ErrorCode::kInternal;
    if (primary == SQLITE_CONSTRAINT) {
        code = ErrorCode::kAlreadyExists;
    } else if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
        code = ErrorCode::kUnavailable;
    }
    const char* detail = database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
    return Status::Error(code, std::string(operation) + "：" + (detail == nullptr ? "未知 SQLite 错误" : detail));
}

}  // namespace

SqliteStatement::SqliteStatement(const SqliteDatabase* database, sqlite3_stmt* statement)
    : database_(database), statement_(statement) {}

SqliteStatement::~SqliteStatement() {
    if (statement_ == nullptr || database_ == nullptr) return;
    std::lock_guard<std::mutex> lock(database_->mutex_);
    sqlite3_finalize(statement_);
}

SqliteStatement::SqliteStatement(SqliteStatement&& other) noexcept
    : database_(other.database_), statement_(other.statement_), last_insert_row_id_(other.last_insert_row_id_) {
    other.database_ = nullptr;
    other.statement_ = nullptr;
    other.last_insert_row_id_ = 0;
}

SqliteStatement& SqliteStatement::operator=(SqliteStatement&& other) noexcept {
    if (this == &other) return *this;
    if (statement_ != nullptr && database_ != nullptr) {
        std::lock_guard<std::mutex> lock(database_->mutex_);
        sqlite3_finalize(statement_);
    }
    database_ = other.database_;
    statement_ = other.statement_;
    last_insert_row_id_ = other.last_insert_row_id_;
    other.database_ = nullptr;
    other.statement_ = nullptr;
    other.last_insert_row_id_ = 0;
    return *this;
}

Status SqliteStatement::BindInt64(int index, std::int64_t value) {
    if (statement_ == nullptr || database_ == nullptr)
        return Status::Error(ErrorCode::kUnavailable, "SQLite 语句已失效");
    std::lock_guard<std::mutex> lock(database_->mutex_);
    if (database_->handle_ == nullptr) return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库已关闭");
    const int result = sqlite3_bind_int64(statement_, index, value);
    return result == SQLITE_OK ? Status::Ok() : database_->Failure(result, "绑定 SQLite 整数失败");
}

Status SqliteStatement::BindInt(int index, int value) {
    if (statement_ == nullptr || database_ == nullptr)
        return Status::Error(ErrorCode::kUnavailable, "SQLite 语句已失效");
    std::lock_guard<std::mutex> lock(database_->mutex_);
    if (database_->handle_ == nullptr) return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库已关闭");
    const int result = sqlite3_bind_int(statement_, index, value);
    return result == SQLITE_OK ? Status::Ok() : database_->Failure(result, "绑定 SQLite 整数失败");
}

Status SqliteStatement::BindText(int index, std::string_view value) {
    if (statement_ == nullptr || database_ == nullptr)
        return Status::Error(ErrorCode::kUnavailable, "SQLite 语句已失效");
    if (value.size() > static_cast<std::string_view::size_type>(std::numeric_limits<int>::max())) {
        return Status::Error(ErrorCode::kInvalidArgument, "SQLite 文本参数长度超过支持范围");
    }
    std::lock_guard<std::mutex> lock(database_->mutex_);
    if (database_->handle_ == nullptr) return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库已关闭");
    const int result =
        sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    return result == SQLITE_OK ? Status::Ok() : database_->Failure(result, "绑定 SQLite 文本失败");
}

Status SqliteStatement::BindNull(int index) {
    if (statement_ == nullptr || database_ == nullptr)
        return Status::Error(ErrorCode::kUnavailable, "SQLite 语句已失效");
    std::lock_guard<std::mutex> lock(database_->mutex_);
    if (database_->handle_ == nullptr) return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库已关闭");
    const int result = sqlite3_bind_null(statement_, index);
    return result == SQLITE_OK ? Status::Ok() : database_->Failure(result, "绑定 SQLite 空值失败");
}

Result<SqliteStep> SqliteStatement::Step() {
    if (statement_ == nullptr || database_ == nullptr) {
        return Result<SqliteStep>::Failure(ErrorCode::kUnavailable, "SQLite 语句已失效");
    }
    std::lock_guard<std::mutex> lock(database_->mutex_);
    if (database_->handle_ == nullptr) {
        return Result<SqliteStep>::Failure(ErrorCode::kUnavailable, "SQLite 数据库已关闭");
    }
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) return Result<SqliteStep>::Success(SqliteStep::kRow);
    if (result == SQLITE_DONE) {
        last_insert_row_id_ = sqlite3_last_insert_rowid(database_->NativeHandle());
        return Result<SqliteStep>::Success(SqliteStep::kDone);
    }
    const Status status = database_->Failure(result, "执行 SQLite 语句失败");
    return Result<SqliteStep>::Failure(status.code, status.message);
}

bool SqliteStatement::IsNull(int column) const {
    if (statement_ == nullptr || database_ == nullptr) return true;
    std::lock_guard<std::mutex> lock(database_->mutex_);
    return database_->handle_ == nullptr || sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

std::int64_t SqliteStatement::ColumnInt64(int column) const {
    if (statement_ == nullptr || database_ == nullptr) return 0;
    std::lock_guard<std::mutex> lock(database_->mutex_);
    if (database_->handle_ == nullptr) return 0;
    return sqlite3_column_int64(statement_, column);
}

int SqliteStatement::ColumnInt(int column) const {
    if (statement_ == nullptr || database_ == nullptr) return 0;
    std::lock_guard<std::mutex> lock(database_->mutex_);
    if (database_->handle_ == nullptr) return 0;
    return sqlite3_column_int(statement_, column);
}

std::string SqliteStatement::ColumnText(int column) const {
    if (statement_ == nullptr || database_ == nullptr) return {};
    std::lock_guard<std::mutex> lock(database_->mutex_);
    if (database_->handle_ == nullptr || sqlite3_column_type(statement_, column) == SQLITE_NULL) return {};
    const auto* value = sqlite3_column_text(statement_, column);
    const int length = sqlite3_column_bytes(statement_, column);
    return value == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(value), length);
}

std::int64_t SqliteStatement::LastInsertRowId() const {
    if (database_ == nullptr) return 0;
    std::lock_guard<std::mutex> lock(database_->mutex_);
    return last_insert_row_id_;
}

SqliteDatabase::SqliteDatabase(std::string path, std::string vfs_name)
    : path_(std::move(path)), vfs_name_(std::move(vfs_name)) {}

SqliteDatabase::~SqliteDatabase() { Close(); }

Status SqliteDatabase::Failure(int result, const char* operation) const {
    return MakeSqliteFailure(handle_, result, operation);
}

Status SqliteDatabase::Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_ != nullptr) return Status::Ok();
    if (path_.empty()) return Status::Error(ErrorCode::kInvalidArgument, "SQLite 数据库路径不能为空");

    sqlite3* database = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX;
    const char* vfs = vfs_name_.empty() ? nullptr : vfs_name_.c_str();
    const int result = sqlite3_open_v2(path_.c_str(), &database, flags, vfs);
    if (result != SQLITE_OK) {
        const Status status = MakeSqliteFailure(database, result, "打开 SQLite 数据库失败");
        if (database != nullptr) sqlite3_close(database);
        return status;
    }
    handle_ = database;
    sqlite3_busy_timeout(handle_, 5000);

    char* error = nullptr;
    const int configure_result =
        sqlite3_exec(handle_, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA;", nullptr,
                     nullptr, &error);
    if (configure_result != SQLITE_OK) {
        const Status status = Failure(configure_result, "配置 SQLite 失败");
        sqlite3_free(error);
        sqlite3_close(handle_);
        handle_ = nullptr;
        return status;
    }
    sqlite3_free(error);
    return Status::Ok();
}

void SqliteDatabase::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_ == nullptr) return;
    sqlite3_close_v2(handle_);
    handle_ = nullptr;
}

bool SqliteDatabase::IsOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handle_ != nullptr;
}

Status SqliteDatabase::Execute(std::string_view sql) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_ == nullptr) return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    const std::string sql_text(sql);
    char* error = nullptr;
    const int result = sqlite3_exec(handle_, sql_text.c_str(), nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const Status status = Failure(result, "执行 SQLite SQL 失败");
        sqlite3_free(error);
        return status;
    }
    sqlite3_free(error);
    return Status::Ok();
}

Result<SqliteStatement> SqliteDatabase::Prepare(std::string_view sql) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_ == nullptr) return Result<SqliteStatement>::Failure(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    const std::string sql_text(sql);
    sqlite3_stmt* statement = nullptr;
    const int result = sqlite3_prepare_v2(handle_, sql_text.c_str(), -1, &statement, nullptr);
    if (result != SQLITE_OK) {
        const Status status = Failure(result, "编译 SQLite SQL 失败");
        if (statement != nullptr) sqlite3_finalize(statement);
        return Result<SqliteStatement>::Failure(status.code, status.message);
    }
    return Result<SqliteStatement>::Success(SqliteStatement(this, statement));
}

Status SqliteDatabase::BeginTransaction() { return Execute("BEGIN IMMEDIATE"); }

Status SqliteDatabase::Commit() { return Execute("COMMIT"); }

Status SqliteDatabase::Rollback() { return Execute("ROLLBACK"); }

}  // namespace voicelife::storage_sqlite
