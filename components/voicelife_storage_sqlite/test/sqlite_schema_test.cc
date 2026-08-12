#include "voicelife/storage_sqlite/sqlite_schema.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::storage_sqlite::SchemaVersion;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteMigration;
using voicelife::storage_sqlite::SqliteSchema;
using voicelife::test::Check;

namespace {

/** @brief 管理 Schema 测试使用的临时数据库文件。 */
struct TemporaryDatabaseFile {
    std::filesystem::path path;

    /** @brief 删除数据库及其附属日志文件。 @return 无。 */
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
    return {.path = std::filesystem::temp_directory_path() / ("voicelife-schema-" + std::to_string(suffix) + ".db")};
}

/** @brief 执行第一个测试迁移。 @param database 已打开数据库。 @return 迁移结果。 */
Status CreateProbeTable(SqliteDatabase& database) {
    return database.Execute("CREATE TABLE schema_probe (value INTEGER NOT NULL)");
}

/** @brief 执行第二个测试迁移。 @param database 已打开数据库。 @return 迁移结果。 */
Status AddProbeColumn(SqliteDatabase& database) {
    return database.Execute("ALTER TABLE schema_probe ADD COLUMN label TEXT NOT NULL DEFAULT ''");
}

/** @brief 执行第三个测试迁移。 @param database 已打开数据库。 @return 迁移结果。 */
Status CreateSecondProbeTable(SqliteDatabase& database) {
    return database.Execute("CREATE TABLE schema_probe_extra (value INTEGER NOT NULL)");
}

/** @brief 执行必然失败的测试迁移。 @param database 已打开数据库。 @return 迁移失败状态。 */
Status FailMigration(SqliteDatabase& database) { return database.Execute("THIS IS NOT SQL"); }

/**
 * @brief 验证目标版本为零时也能完成数据库健康初始化。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckEmptyInitialization(const std::filesystem::path& path) {
    SqliteDatabase unopened(path.string());
    Check(SqliteSchema::ReadVersion(unopened).status.code == ErrorCode::kUnavailable,
          "未打开数据库不能读取 Schema 版本");
    Check(SqliteSchema::QuickCheck(unopened).code == ErrorCode::kUnavailable, "未打开数据库不能执行 quick_check");

    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "Schema 测试应打开数据库");
    Check(SqliteSchema::Initialize(database).ok(), "无迁移初始化应成功");
    const auto version = SqliteSchema::ReadVersion(database);
    Check(version.ok() && *version.value == 0, "空 Schema 版本应为零");
    Check(SqliteSchema::QuickCheck(database).ok(), "空数据库 quick_check 应成功");
}

/**
 * @brief 验证连续迁移、幂等调用和重连后的版本持久化。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckSuccessfulMigrations(const std::filesystem::path& path) {
    const SqliteMigration migrations[] = {
        {.version = 1, .apply = &CreateProbeTable},
        {.version = 2, .apply = &AddProbeColumn},
    };
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "迁移测试应打开数据库");
    Check(SqliteSchema::Initialize(database, 2, migrations, 2).ok(), "连续迁移应成功");
    const auto version = SqliteSchema::ReadVersion(database);
    Check(version.ok() && *version.value == 2, "迁移后版本应为二");
    Check(SqliteSchema::ApplyMigrations(database, 2, migrations, 2).ok(), "重复迁移应幂等");
    Check(SqliteSchema::QuickCheck(database).ok(), "迁移后 quick_check 应成功");
    database.Close();

    Check(database.Open().ok(), "迁移数据库应支持重连");
    const auto reopened = SqliteSchema::ReadVersion(database);
    Check(reopened.ok() && *reopened.value == 2, "重连后应保留 Schema 版本");

    const SqliteMigration future_migration[] = {{.version = 3, .apply = &CreateSecondProbeTable}};
    Check(SqliteSchema::ApplyMigrations(database, 3, future_migration, 1).ok(), "从当前版本开始的迁移子集应成功");
    const auto upgraded = SqliteSchema::ReadVersion(database);
    Check(upgraded.ok() && *upgraded.value == 3, "迁移子集完成后版本应为三");
}

/**
 * @brief 验证迁移失败时版本和已创建对象均回滚。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckMigrationRollback(const std::filesystem::path& path) {
    const SqliteMigration migrations[] = {
        {.version = 1, .apply = &CreateProbeTable},
        {.version = 2, .apply = &FailMigration},
    };
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "回滚测试应打开数据库");
    const Status failed = SqliteSchema::ApplyMigrations(database, 2, migrations, 2);
    Check(!failed.ok(), "失败迁移应返回错误");
    const auto version = SqliteSchema::ReadVersion(database);
    Check(version.ok() && *version.value == 0, "失败迁移后版本应回滚到零");
    auto table = database.Prepare("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='schema_probe'");
    Check(table.ok() && table.value->Step().ok() && table.value->ColumnInt(0) == 0, "失败迁移创建的表也应回滚");
}

/**
 * @brief 验证非法迁移描述和降级请求被拒绝。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckMigrationValidation(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "校验测试应打开数据库");
    const SqliteMigration gap[] = {{.version = 2, .apply = &CreateProbeTable}};
    Check(SqliteSchema::ApplyMigrations(database, 2, gap, 1).code == ErrorCode::kInvalidArgument,
          "缺少版本一的迁移应被拒绝");
    const SqliteMigration internal_gap[] = {
        {.version = 1, .apply = &CreateProbeTable},
        {.version = 3, .apply = &AddProbeColumn},
    };
    Check(SqliteSchema::ApplyMigrations(database, 3, internal_gap, 2).code == ErrorCode::kInvalidArgument,
          "迁移列表内部缺少连续版本应被拒绝");
    Check(SqliteSchema::ApplyMigrations(database, 0, nullptr, 0).ok(), "无迁移且目标为零应保持幂等");
    const SqliteMigration null_callback[] = {{.version = 1, .apply = nullptr}};
    Check(SqliteSchema::ApplyMigrations(database, 1, null_callback, 1).code == ErrorCode::kInvalidArgument,
          "空迁移回调应被拒绝");
    Check(SqliteSchema::ApplyMigrations(database, std::numeric_limits<SchemaVersion>::max(), nullptr, 0).code ==
              ErrorCode::kInvalidArgument,
          "超出 user_version 范围的目标版本应被拒绝");
    Check(database.Execute("PRAGMA user_version = 3").ok(), "应能构造高版本数据库");
    Check(SqliteSchema::ApplyMigrations(database, 2, nullptr, 0).code == ErrorCode::kConflict, "降级请求应被拒绝");
}

/** @brief 执行 SQLite Schema 基础设施单元测试。 @return 全部断言通过时返回 0。 */
int RunTests() {
    const TemporaryDatabaseFile empty = MakeTemporaryDatabaseFile();
    CheckEmptyInitialization(empty.path);
    const TemporaryDatabaseFile success = MakeTemporaryDatabaseFile();
    CheckSuccessfulMigrations(success.path);
    const TemporaryDatabaseFile rollback = MakeTemporaryDatabaseFile();
    CheckMigrationRollback(rollback.path);
    const TemporaryDatabaseFile validation = MakeTemporaryDatabaseFile();
    CheckMigrationValidation(validation.path);
    return 0;
}

}  // namespace

/** @brief 执行 SQLite Schema 测试入口。 @return 全部断言通过时返回 0。 */
int main() { return RunTests(); }
