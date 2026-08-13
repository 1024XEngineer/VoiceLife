#pragma once

#include <cstddef>
#include <cstdint>

#include "voicelife/contracts/status.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

namespace voicelife::storage_sqlite {

/** @brief SQLite 持久化格式使用的非负版本号。 */
using SchemaVersion = std::uint32_t;

/**
 * @brief 定义单个 SQLite Schema 迁移步骤的执行函数类型。
 * @param database 已打开且已进入迁移事务的数据库连接。
 * @return 迁移成功时返回成功状态，否则返回应触发回滚的错误状态。
 */
using MigrationCallback = Status (*)(SqliteDatabase& database);

/**
 * @brief 描述一个从上一个版本升级到当前版本的 Schema 迁移步骤。
 *
 * 迁移回调只负责执行当前版本所需的 DDL/DML，不应自行开始、提交或回滚事务，
 * 也不应直接修改 PRAGMA user_version；版本标记由 SqliteSchema 统一写入。
 */
struct SqliteMigration {
    /** @brief 迁移完成后对应的非零 Schema 版本；迁移数组中的版本必须严格递增。 */
    SchemaVersion version = 0;
    /** @brief 执行该版本迁移的回调函数。 */
    MigrationCallback apply = nullptr;
};

/**
 * @brief 提供与业务无关的 SQLite Schema 版本、迁移和完整性检查能力。
 *
 * 该类使用 SQLite 内置的 PRAGMA user_version 保存版本，不创建额外元数据表。
 * 它只负责数据库基础设施，业务表的定义应通过 SqliteMigration 回调提供。
 */
class SqliteSchema final {
   public:
    /**
     * @brief 读取数据库当前 Schema 版本。
     * @param database 已打开的 SQLite 数据库连接。
     * @return 当前版本号；数据库未打开或读取失败时返回错误。
     */
    [[nodiscard]] static Result<SchemaVersion> ReadVersion(const SqliteDatabase& database);

    /**
     * @brief 在事务中执行迁移并将数据库升级到目标版本。
     *
     * 迁移列表必须按版本严格递增，并覆盖当前版本到目标版本之间的每个连续版本；
     * 列表可以是从版本一开始的全量列表，也可以只包含当前版本之后的连续子集。
     * 版本受 SQLite user_version 限制，不能超过 32 位有符号整数上限。
     * 目标版本等于当前版本时不会开启事务，也不会执行任何回调。
     * @param database 已打开的 SQLite 数据库连接。
     * @param target_version 固件支持的目标 Schema 版本。
     * @param migrations 按版本递增排列的迁移列表；无迁移时可传 nullptr。
     * @param migration_count 迁移列表元素数量。
     * @return 全部迁移提交成功时返回成功状态，否则返回错误并回滚事务。
     */
    static Status ApplyMigrations(SqliteDatabase& database, SchemaVersion target_version,
                                  const SqliteMigration* migrations, std::size_t migration_count);

    /**
     * @brief 执行 SQLite quick_check 完整性检查。
     * @param database 已打开的 SQLite 数据库连接。
     * @return 检查结果为 ok 时返回成功，否则返回数据库损坏或执行错误。
     */
    [[nodiscard]] static Status QuickCheck(const SqliteDatabase& database);

    /**
     * @brief 初始化 Schema 并执行完整性检查。
     *
     * 目标版本默认为 0，因此 Runtime 在尚未接入业务表时也可以调用此方法完成
     * 数据库可用性验证；有迁移时先完成迁移提交，再执行 quick_check。
     * @param database 已打开的 SQLite 数据库连接。
     * @param target_version 固件支持的目标 Schema 版本，默认值为 0。
     * @param migrations 按版本递增排列的迁移列表；无迁移时可传 nullptr。
     * @param migration_count 迁移列表元素数量，默认值为 0。
     * @return Schema 初始化和完整性检查均成功时返回成功状态。
     */
    static Status Initialize(SqliteDatabase& database, SchemaVersion target_version = 0,
                             const SqliteMigration* migrations = nullptr, std::size_t migration_count = 0);

   private:
    /** @brief 该类型只提供静态基础设施操作，禁止实例化。 */
    SqliteSchema() = delete;
};

}  // namespace voicelife::storage_sqlite
