#include "voicelife/storage_sqlite/sqlite_database.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteStatement;
using voicelife::storage_sqlite::SqliteStep;
using voicelife::test::Check;

namespace {

/** @brief 管理 Database 单元测试使用的临时数据库文件。 */
struct TemporaryDatabaseFile {
    std::filesystem::path path;

    /** @brief 删除数据库及其日志文件。 @return 无。 */
    ~TemporaryDatabaseFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-journal", error);
        std::filesystem::remove(path.string() + "-wal", error);
        std::filesystem::remove(path.string() + "-shm", error);
    }
};

/** @brief 创建唯一临时数据库路径。 @return 尚不存在的 SQLite 文件路径。 */
TemporaryDatabaseFile MakeTemporaryDatabaseFile() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return {.path = std::filesystem::temp_directory_path() / ("voicelife-database-" + std::to_string(suffix) + ".db")};
}

/**
 * @brief 通过别名覆盖移动赋值的自赋值保护分支。
 * @param statement 待自赋值的语句。
 * @return 无。
 */
void MoveAssignSelf(SqliteStatement& statement) {
    SqliteStatement* alias = &statement;
    statement = std::move(*alias);
}

/**
 * @brief 读取只返回一个整数的查询。
 * @param database 已打开数据库。
 * @param sql 查询 SQL。
 * @return 查询返回的整数。
 */
int64_t ScalarInt64(SqliteDatabase& database, const char* sql) {
    auto prepared = database.Prepare(sql);
    Check(prepared.ok(), "标量查询应成功编译");
    const auto row = prepared.value->Step();
    Check(row.ok() && *row.value == SqliteStep::kRow, "标量查询应返回一行");
    const int64_t value = prepared.value->ColumnInt64(0);
    const auto done = prepared.value->Step();
    Check(done.ok() && *done.value == SqliteStep::kDone, "标量查询应在一行后结束");
    return value;
}

/**
 * @brief 验证未打开、非法路径和非法 VFS 的错误。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckOpenAndClosedStates(const std::filesystem::path& path) {
    SqliteDatabase unopened(path.string());
    Check(unopened.path() == path.string() && !unopened.IsOpen(), "构造后应保留路径且尚未打开");
    Check(unopened.Execute("SELECT 1").code == ErrorCode::kUnavailable, "未打开数据库不能执行 SQL");
    Check(unopened.Prepare("SELECT 1").status.code == ErrorCode::kUnavailable, "未打开数据库不能编译 SQL");
    unopened.Close();

    SqliteDatabase empty_path("");
    Check(empty_path.Open().code == ErrorCode::kInvalidArgument, "空数据库路径应被拒绝");

    const std::filesystem::path missing_directory = path.string() + "-missing";
    SqliteDatabase missing_parent((missing_directory / "database.db").string());
    Check(missing_parent.Open().code == ErrorCode::kInternal, "不存在的父目录应返回打开错误");

    SqliteDatabase missing_vfs(path.string(), "voicelife-missing-vfs");
    Check(missing_vfs.Open().code == ErrorCode::kInternal, "不存在的 VFS 应返回打开错误");
}

/**
 * @brief 验证基础执行、事务、重复打开和重复关闭。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckExecutionAndTransactions(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok() && database.Open().ok() && database.IsOpen(), "打开操作应成功且保持幂等");
    Check(database.Execute("CREATE TABLE transaction_probe (value INTEGER NOT NULL)").ok(), "应成功建表");
    Check(database.Execute("THIS IS NOT SQL").code == ErrorCode::kInternal, "非法 SQL 应映射为内部错误");
    Check(database.Prepare("SELECT FROM").status.code == ErrorCode::kInternal, "非法查询应返回编译错误");
    Check(database.Commit().code == ErrorCode::kInternal, "无事务时提交应失败");
    Check(database.Rollback().code == ErrorCode::kInternal, "无事务时回滚应失败");

    Check(database.BeginTransaction().ok(), "应成功开始回滚事务");
    Check(database.Execute("INSERT INTO transaction_probe VALUES (1)").ok(), "事务内写入应成功");
    Check(database.Rollback().ok(), "应成功回滚事务");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM transaction_probe") == 0, "回滚后不应保留写入");

    Check(database.BeginTransaction().ok(), "应成功开始提交事务");
    Check(database.Execute("INSERT INTO transaction_probe VALUES (2)").ok(), "提交前写入应成功");
    Check(database.Commit().ok(), "应成功提交事务");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM transaction_probe") == 1, "提交后应保留写入");

    database.Close();
    Check(!database.IsOpen(), "关闭后连接状态应失效");
    database.Close();
    Check(database.Open().ok(), "关闭后的数据库应允许重新打开");
}

/**
 * @brief 验证所有绑定和列读取类型。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckBindingsAndColumns(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "绑定测试应打开数据库");
    Check(database.Execute("CREATE TABLE binding_probe (wide INTEGER, narrow INTEGER, text_value TEXT, optional TEXT)")
              .ok(),
          "应成功创建绑定测试表");

    auto inserted = database.Prepare("INSERT INTO binding_probe VALUES (?, ?, ?, ?)");
    Check(inserted.ok(), "应成功编译绑定写入");
    Check(inserted.value->BindInt64(1, INT64_C(5'000'000'000)).ok(), "应成功绑定 64 位整数");
    Check(inserted.value->BindInt(2, 42).ok(), "应成功绑定整数");
    Check(inserted.value->BindText(3, "字段值").ok(), "应成功绑定文本");
    Check(inserted.value->BindNull(4).ok(), "应成功绑定空值");
    Check(inserted.value->BindInt64(9, 1).code == ErrorCode::kInternal, "越界 64 位整数参数应失败");
    Check(inserted.value->BindInt(9, 1).code == ErrorCode::kInternal, "越界整数参数应失败");
    Check(inserted.value->BindText(9, "x").code == ErrorCode::kInternal, "越界文本参数应失败");
    Check(inserted.value->BindNull(9).code == ErrorCode::kInternal, "越界空值参数应失败");
    const auto inserted_step = inserted.value->Step();
    Check(inserted_step.ok() && *inserted_step.value == SqliteStep::kDone, "绑定写入应执行完成");

    auto selected = database.Prepare("SELECT wide, narrow, text_value, optional FROM binding_probe");
    Check(selected.ok(), "应成功编译绑定结果查询");
    const auto row = selected.value->Step();
    Check(row.ok() && *row.value == SqliteStep::kRow, "绑定结果应返回一行");
    Check(!selected.value->IsNull(0) && selected.value->IsNull(3), "应区分普通列和 NULL 列");
    Check(selected.value->ColumnInt64(0) == INT64_C(5'000'000'000), "应读取 64 位整数列");
    Check(selected.value->ColumnInt(1) == 42, "应读取整数列");
    Check(selected.value->ColumnText(2) == "字段值" && selected.value->ColumnText(3).empty(),
          "应读取文本并把 NULL 文本映射为空字符串");
}

/**
 * @brief 验证 Statement 移动语义和关闭连接后的防御行为。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckStatementLifecycle(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "Statement 生命周期测试应打开数据库");
    auto first = database.Prepare("SELECT 1");
    auto second = database.Prepare("SELECT 2");
    Check(first.ok() && second.ok(), "应成功创建待移动 Statement");

    SqliteStatement moved = std::move(*first.value);
    Check(first.value->BindInt64(1, 1).code == ErrorCode::kUnavailable, "移动后的 Statement 不能绑定整数");
    Check(first.value->BindInt(1, 1).code == ErrorCode::kUnavailable, "移动后的 Statement 不能绑定普通整数");
    Check(first.value->BindText(1, "x").code == ErrorCode::kUnavailable, "移动后的 Statement 不能绑定文本");
    Check(first.value->BindNull(1).code == ErrorCode::kUnavailable, "移动后的 Statement 不能绑定空值");
    Check(first.value->Step().status.code == ErrorCode::kUnavailable, "移动后的 Statement 不能执行");
    Check(first.value->IsNull(0) && first.value->ColumnInt64(0) == 0 && first.value->ColumnInt(0) == 0 &&
              first.value->ColumnText(0).empty() && first.value->LastInsertRowId() == 0,
          "移动后的 Statement 读取应返回安全默认值");

    MoveAssignSelf(moved);
    moved = std::move(*second.value);
    const auto moved_row = moved.Step();
    Check(moved_row.ok() && moved.ColumnInt(0) == 2, "移动赋值后应执行新的 Statement");

    database.Close();
    Check(moved.BindInt64(1, 1).code == ErrorCode::kUnavailable, "关闭连接后不能绑定 64 位整数");
    Check(moved.BindInt(1, 1).code == ErrorCode::kUnavailable, "关闭连接后不能绑定整数");
    Check(moved.BindText(1, "x").code == ErrorCode::kUnavailable, "关闭连接后不能绑定文本");
    Check(moved.BindNull(1).code == ErrorCode::kUnavailable, "关闭连接后不能绑定空值");
    Check(moved.Step().status.code == ErrorCode::kUnavailable, "关闭连接后不能执行 Statement");
    Check(moved.IsNull(0) && moved.ColumnInt64(0) == 0 && moved.ColumnInt(0) == 0 && moved.ColumnText(0).empty(),
          "关闭连接后的列读取应返回安全默认值");
}

/**
 * @brief 验证约束错误、Step 错误和数据库锁错误映射。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckSqliteErrorMapping(const std::filesystem::path& path) {
    SqliteDatabase first(path.string());
    SqliteDatabase second(path.string());
    Check(first.Open().ok() && second.Open().ok(), "错误映射测试应打开两个连接");
    Check(first.Execute("CREATE TABLE error_probe (value INTEGER UNIQUE)").ok(), "应成功创建错误测试表");
    Check(first.Execute("INSERT INTO error_probe VALUES (1)").ok(), "第一次唯一值写入应成功");
    Check(first.Execute("INSERT INTO error_probe VALUES (1)").code == ErrorCode::kAlreadyExists,
          "唯一约束错误应映射为已存在");

    auto duplicate = first.Prepare("INSERT INTO error_probe VALUES (?)");
    Check(duplicate.ok() && duplicate.value->BindInt(1, 1).ok(), "应成功准备重复值写入");
    Check(duplicate.value->Step().status.code == ErrorCode::kAlreadyExists, "Statement 唯一约束错误应映射为已存在");

    Check(second.Execute("PRAGMA busy_timeout=0").ok(), "第二连接应关闭 busy 等待");
    Check(first.BeginTransaction().ok(), "第一连接应取得写事务");
    Check(first.Execute("INSERT INTO error_probe VALUES (2)").ok(), "第一连接事务写入应成功");
    Check(second.Execute("INSERT INTO error_probe VALUES (3)").code == ErrorCode::kUnavailable,
          "锁冲突应映射为暂不可用");
    Check(first.Rollback().ok(), "锁冲突测试后应回滚事务");
}

}  // namespace

/** @brief 执行 SQLite Database/Statement 单元测试。 @return 全部断言通过时返回 0。 */
int main() {
    const TemporaryDatabaseFile open_states = MakeTemporaryDatabaseFile();
    CheckOpenAndClosedStates(open_states.path);
    const TemporaryDatabaseFile transactions = MakeTemporaryDatabaseFile();
    CheckExecutionAndTransactions(transactions.path);
    const TemporaryDatabaseFile bindings = MakeTemporaryDatabaseFile();
    CheckBindingsAndColumns(bindings.path);
    const TemporaryDatabaseFile lifecycle = MakeTemporaryDatabaseFile();
    CheckStatementLifecycle(lifecycle.path);
    const TemporaryDatabaseFile errors = MakeTemporaryDatabaseFile();
    CheckSqliteErrorMapping(errors.path);
    return 0;
}
