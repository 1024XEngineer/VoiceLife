#include "voicelife/storage_sqlite/sqlite_schema.h"

#include <limits>
#include <string>
#include <utility>

namespace voicelife::storage_sqlite {
namespace {

constexpr SchemaVersion kMaximumSchemaVersion = std::numeric_limits<std::int32_t>::max();

/**
 * @brief 创建迁移失败后的回滚错误状态。
 * @param migration_status 原始迁移错误。
 * @param rollback_status 回滚错误。
 * @return 保留原始错误并附加回滚信息的状态。
 */
Status MigrationFailure(const Status& migration_status, const Status& rollback_status) {
    if (rollback_status.ok()) return migration_status;
    return Status::Error(migration_status.code,
                         migration_status.message + "；迁移回滚失败：" + rollback_status.message);
}

/**
 * @brief 校验迁移描述数组的静态约束。
 * @param migrations 待校验的迁移数组。
 * @param migration_count 数组元素数量。
 * @return 描述有效时返回成功状态。
 */
Status ValidateMigrations(const SqliteMigration* migrations, std::size_t migration_count) {
    if (migration_count != 0 && migrations == nullptr) {
        return Status::Error(ErrorCode::kInvalidArgument, "SQLite 迁移数量非零时必须提供迁移列表");
    }
    SchemaVersion previous = 0;
    for (std::size_t index = 0; index < migration_count; ++index) {
        const SqliteMigration& migration = migrations[index];
        if (migration.version == 0 || migration.version > kMaximumSchemaVersion || migration.version <= previous) {
            return Status::Error(ErrorCode::kInvalidArgument, "SQLite 迁移版本必须为非零且严格递增");
        }
        if (migration.apply == nullptr) {
            return Status::Error(ErrorCode::kInvalidArgument, "SQLite 迁移缺少执行回调");
        }
        previous = migration.version;
    }
    return Status::Ok();
}

/**
 * @brief 将无效的 SQLite 版本值转换为项目状态。
 * @param value SQLite 返回的版本整数。
 * @return 版本有效时返回成功结果，否则返回内部错误。
 */
Result<SchemaVersion> ConvertVersion(std::int64_t value) {
    if (value < 0 || static_cast<std::uint64_t>(value) > kMaximumSchemaVersion) {
        return Result<SchemaVersion>::Failure(ErrorCode::kInternal, "SQLite Schema 版本值无效");
    }
    return Result<SchemaVersion>::Success(static_cast<SchemaVersion>(value));
}

/**
 * @brief 查找当前版本之后的第一个迁移并验证目标版本覆盖关系。
 * @param current_version 数据库当前版本。
 * @param target_version 目标版本。
 * @param migrations 迁移数组。
 * @param migration_count 数组元素数量。
 * @param start_index 输出第一个待执行迁移下标。
 * @return 迁移范围有效时返回成功状态。
 */
Status FindMigrationRange(SchemaVersion current_version, SchemaVersion target_version,
                          const SqliteMigration* migrations, std::size_t migration_count, std::size_t& start_index) {
    start_index = 0;
    while (start_index < migration_count && migrations[start_index].version <= current_version) ++start_index;

    SchemaVersion expected = current_version + 1;
    for (std::size_t index = start_index; index < migration_count && migrations[index].version <= target_version;
         ++index) {
        if (migrations[index].version != expected) {
            return Status::Error(ErrorCode::kInvalidArgument, "SQLite 迁移列表未覆盖连续 Schema 版本");
        }
        ++expected;
    }
    if (expected != target_version + 1) {
        return Status::Error(ErrorCode::kInvalidArgument, "SQLite 迁移列表缺少目标 Schema 版本");
    }
    return Status::Ok();
}

}  // namespace

Result<SchemaVersion> SqliteSchema::ReadVersion(const SqliteDatabase& database) {
    if (!database.IsOpen()) {
        return Result<SchemaVersion>::Failure(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");
    }

    Result<SqliteStatement> prepared = database.Prepare("PRAGMA user_version");
    if (!prepared.ok()) return Result<SchemaVersion>::Failure(prepared.status.code, prepared.status.message);
    SqliteStatement statement = std::move(*prepared.value);
    const Result<SqliteStep> row = statement.Step();
    if (!row.ok()) return Result<SchemaVersion>::Failure(row.status.code, row.status.message);
    if (*row.value != SqliteStep::kRow) {
        return Result<SchemaVersion>::Failure(ErrorCode::kInternal, "读取 SQLite Schema 版本未返回结果");
    }
    const Result<SchemaVersion> version = ConvertVersion(statement.ColumnInt64(0));
    if (!version.ok()) return version;
    const Result<SqliteStep> done = statement.Step();
    if (!done.ok()) return Result<SchemaVersion>::Failure(done.status.code, done.status.message);
    if (*done.value != SqliteStep::kDone) {
        return Result<SchemaVersion>::Failure(ErrorCode::kInternal, "读取 SQLite Schema 版本返回多行结果");
    }
    return version;
}

Status SqliteSchema::ApplyMigrations(SqliteDatabase& database, SchemaVersion target_version,
                                     const SqliteMigration* migrations, std::size_t migration_count) {
    const Status descriptions = ValidateMigrations(migrations, migration_count);
    if (!descriptions.ok()) return descriptions;
    if (target_version > kMaximumSchemaVersion) {
        return Status::Error(ErrorCode::kInvalidArgument, "SQLite Schema 目标版本超出 user_version 支持范围");
    }

    const Result<SchemaVersion> current = ReadVersion(database);
    if (!current.ok()) return current.status;
    if (*current.value > target_version) {
        return Status::Error(ErrorCode::kConflict, "SQLite 数据库 Schema 版本高于当前固件支持版本");
    }
    if (*current.value == target_version) return Status::Ok();
    std::size_t start_index = 0;
    const Status range = FindMigrationRange(*current.value, target_version, migrations, migration_count, start_index);
    if (!range.ok()) return range;

    const Status begin = database.BeginTransaction();
    if (!begin.ok()) return begin;

    SchemaVersion version = *current.value;
    for (std::size_t index = start_index; index < migration_count && migrations[index].version <= target_version;
         ++index) {
        const SqliteMigration& migration = migrations[index];
        const Status applied = migration.apply(database);
        if (!applied.ok()) {
            return MigrationFailure(applied, database.Rollback());
        }
        version = migration.version;
        const std::string set_version = "PRAGMA user_version = " + std::to_string(version);
        const Status marked = database.Execute(set_version);
        if (!marked.ok()) return MigrationFailure(marked, database.Rollback());
    }

    const Status committed = database.Commit();
    if (!committed.ok()) return MigrationFailure(committed, database.Rollback());
    return Status::Ok();
}

Status SqliteSchema::QuickCheck(const SqliteDatabase& database) {
    if (!database.IsOpen()) return Status::Error(ErrorCode::kUnavailable, "SQLite 数据库尚未打开");

    Result<SqliteStatement> prepared = database.Prepare("PRAGMA quick_check");
    if (!prepared.ok()) return prepared.status;
    SqliteStatement statement = std::move(*prepared.value);
    bool has_row = false;
    while (true) {
        const Result<SqliteStep> stepped = statement.Step();
        if (!stepped.ok()) return stepped.status;
        if (*stepped.value == SqliteStep::kDone) break;
        has_row = true;
        if (statement.IsNull(0) || statement.ColumnText(0) != "ok") {
            return Status::Error(ErrorCode::kInternal, "SQLite quick_check 检测到数据库完整性错误");
        }
    }
    return has_row ? Status::Ok() : Status::Error(ErrorCode::kInternal, "SQLite quick_check 未返回结果");
}

Status SqliteSchema::Initialize(SqliteDatabase& database, SchemaVersion target_version,
                                const SqliteMigration* migrations, std::size_t migration_count) {
    const Status migrated = ApplyMigrations(database, target_version, migrations, migration_count);
    if (!migrated.ok()) return migrated;
    return QuickCheck(database);
}

}  // namespace voicelife::storage_sqlite
