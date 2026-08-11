#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"

/** @brief SQLite C API 的连接句柄前置声明。 */
struct sqlite3;

/** @brief SQLite C API 的预编译语句句柄前置声明。 */
struct sqlite3_stmt;

namespace voicelife::storage_sqlite {

/** @brief SQLite 数据库基础设施前置声明。 */
class SqliteDatabase;

/** @brief 表示一次 SQLite 语句执行后的结果类型。 */
enum class SqliteStep { kRow, kDone };

/**
 * @brief 封装 SQLite 预编译语句和参数绑定。
 *
 * Statement 不长期占用 Database 的连接锁；每次原生 SQLite 操作只在调用期间加锁，
 * 因此事务提交、回滚和关闭不会因为语句对象仍在作用域内而自锁。语句对象必须在所属
 * Database 关闭或析构前销毁。
 */
class SqliteStatement {
   public:
    /** @brief 释放预编译语句。 */
    ~SqliteStatement();

    /** @brief 语句不能复制。 */
    SqliteStatement(const SqliteStatement&) = delete;
    /** @brief 语句不能复制赋值。 */
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    /**
     * @brief 允许转移语句所有权。
     * @param other 待接管的语句。
     */
    SqliteStatement(SqliteStatement&& other) noexcept;
    /**
     * @brief 允许转移语句所有权赋值。
     * @param other 待接管的语句。
     * @return 当前语句引用。
     */
    SqliteStatement& operator=(SqliteStatement&& other) noexcept;

    /**
     * @brief 绑定 64 位整数参数。
     * @param index SQLite 参数序号，从 1 开始。
     * @param value 要绑定的值。
     * @return 绑定成功时返回成功状态。
     */
    Status BindInt64(int index, std::int64_t value);

    /**
     * @brief 绑定 32 位整数参数。
     * @param index SQLite 参数序号，从 1 开始。
     * @param value 要绑定的值。
     * @return 绑定成功时返回成功状态。
     */
    Status BindInt(int index, int value);

    /**
     * @brief 绑定文本参数。
     * @param index SQLite 参数序号，从 1 开始。
     * @param value 要绑定的文本。
     * @return 绑定成功时返回成功状态。
     */
    Status BindText(int index, std::string_view value);

    /**
     * @brief 绑定空值参数。
     * @param index SQLite 参数序号，从 1 开始。
     * @return 绑定成功时返回成功状态。
     */
    Status BindNull(int index);

    /**
     * @brief 执行语句一步。
     * @return 有数据行、执行完成或 SQLite 错误。
     */
    [[nodiscard]] Result<SqliteStep> Step();

    /**
     * @brief 判断结果列是否为 SQL NULL。
     * @param column 结果列序号，从 0 开始。
     * @return 列为空时返回 true。
     */
    [[nodiscard]] bool IsNull(int column) const;

    /**
     * @brief 读取结果中的 64 位整数列。
     * @param column 结果列序号，从 0 开始。
     * @return 列值。
     */
    [[nodiscard]] std::int64_t ColumnInt64(int column) const;

    /**
     * @brief 读取结果中的整数列。
     * @param column 结果列序号，从 0 开始。
     * @return 列值。
     */
    [[nodiscard]] int ColumnInt(int column) const;

    /**
     * @brief 复制结果中的文本列。
     * @param column 结果列序号，从 0 开始。
     * @return 列文本；SQL NULL 返回空字符串。
     */
    [[nodiscard]] std::string ColumnText(int column) const;

    /**
     * @brief 返回本语句最近一次完成写入时缓存的自增标识。
     * @return 本语句对应的最近插入行标识。
     */
    [[nodiscard]] std::int64_t LastInsertRowId() const;

   private:
    friend class SqliteDatabase;

    /**
     * @brief 构造一个已编译的语句包装器。
     * @param database 所属数据库。
     * @param statement SQLite 原生语句句柄。
     */
    SqliteStatement(const SqliteDatabase* database, sqlite3_stmt* statement);

    const SqliteDatabase* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
    std::int64_t last_insert_row_id_ = 0;
};

/**
 * @brief 管理 SQLite 连接、基础配置、语句和事务生命周期。
 *
 * 该类不包含 Schedule 或其他业务类型，只提供数据库基础设施能力。
 * 连接锁只串行化单次调用，不调度跨调用事务；事务必须由同一个控制面写入者完整执行。
 */
class SqliteDatabase {
   public:
    /**
     * @brief 创建数据库连接管理器。
     * @param path SQLite 数据库文件路径或 URI。
     * @param vfs_name 可选的 SQLite VFS 名称；主机环境通常为空。
     */
    explicit SqliteDatabase(std::string path, std::string vfs_name = {});

    /** @brief 关闭数据库连接并释放资源。 */
    ~SqliteDatabase();

    /** @brief 数据库连接不能复制。 */
    SqliteDatabase(const SqliteDatabase&) = delete;
    /** @brief 数据库连接不能复制赋值。 */
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;
    /** @brief 数据库连接不能移动。 */
    SqliteDatabase(SqliteDatabase&&) = delete;
    /** @brief 数据库连接不能移动赋值。 */
    SqliteDatabase& operator=(SqliteDatabase&&) = delete;

    /**
     * @brief 打开数据库并设置基础连接参数。
     * @return 打开成功时返回成功状态，否则返回数据库错误。
     */
    Status Open();

    /** @brief 关闭当前连接；重复调用是安全的。 */
    void Close();

    /**
     * @brief 判断数据库连接是否已经打开。
     * @return 连接有效时返回 true。
     */
    [[nodiscard]] bool IsOpen() const;

    /**
     * @brief 执行不需要返回行的基础 SQL。
     * @param sql 建表、迁移或配置 SQL 文本。
     * @return 执行成功时返回成功状态，否则返回数据库错误。
     */
    Status Execute(std::string_view sql);

    /**
     * @brief 编译一条预处理 SQL 并返回语句包装器。
     * @param sql SQL 文本。
     * @return 可绑定和执行的语句，或编译错误。
     */
    [[nodiscard]] Result<SqliteStatement> Prepare(std::string_view sql) const;

    /**
     * @brief 开始一个立即写事务。
     * @return 开始成功时返回成功状态。
     */
    Status BeginTransaction();

    /**
     * @brief 提交当前事务。
     * @return 提交成功时返回成功状态。
     */
    Status Commit();

    /**
     * @brief 回滚当前事务。
     * @return 回滚成功时返回成功状态。
     */
    Status Rollback();

    /**
     * @brief 返回数据库路径。
     * @return 构造时传入的路径引用。
     */
    [[nodiscard]] const std::string& path() const { return path_; }

   private:
    friend class SqliteStatement;

    /**
     * @brief 将 SQLite 返回码转换为项目错误状态。
     * @param result SQLite 返回码。
     * @param operation 失败操作说明。
     * @return 映射后的错误状态。
     */
    Status Failure(int result, const char* operation) const;

    /** @brief 返回原生连接句柄，仅供 Statement 内部使用。 */
    [[nodiscard]] sqlite3* NativeHandle() const { return handle_; }

    std::string path_;
    std::string vfs_name_;
    sqlite3* handle_ = nullptr;
    mutable std::mutex mutex_;
};

}  // namespace voicelife::storage_sqlite
