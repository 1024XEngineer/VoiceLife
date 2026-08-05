#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "voicelife/contracts/status.h"

namespace voicelife::storage_sqlite {

/** 表示 SQLite 协议允许绑定的标量或二进制值。 */
using StorageValue = std::variant<std::monostate, std::int64_t, double, bool, std::string, std::vector<std::uint8_t>>;

/** 表示一个由领域适配器拥有的命名存储语句。 */
struct StorageStatement {
    std::string name;
    std::vector<StorageValue> arguments;

    /**
     * @brief 校验命名语句标识和参数形状。
     * @return 语句有效时返回成功状态。
     */
    [[nodiscard]] Status Validate() const;
};

/** 保存存储请求的幂等标识和截止时间。 */
struct StorageRequestContext {
    std::string request_id;
    std::uint32_t deadline_ms = 0;

    /**
     * @brief 校验请求标识和截止时间。
     * @return 上下文有效时返回成功状态。
     */
    [[nodiscard]] Status Validate() const;
};

/** 表示一次只读命名语句请求。 */
struct StorageReadRequest {
    StorageRequestContext context;
    StorageStatement query;

    /**
     * @brief 校验只读请求上下文和查询语句。
     * @return 请求有效时返回成功状态。
     */
    [[nodiscard]] Status Validate() const;
};

/** 表示一次原子提交的命名语句批次。 */
struct StorageWriteRequest {
    StorageRequestContext context;
    std::vector<StorageStatement> statements;

    /**
     * @brief 校验写请求上下文和语句批次。
     * @return 请求有效时返回成功状态。
     */
    [[nodiscard]] Status Validate() const;
};

/** 表示查询结果中的一行协议值。 */
struct StorageRow {
    std::vector<StorageValue> values;
};

/** 表示带快照版本的读取结果。 */
struct StorageReadResult {
    std::string request_id;
    std::vector<StorageRow> rows;
    std::uint64_t snapshot_revision = 0;
};

/** 表示一次写事务的提交回执和幂等信息。 */
struct StorageWriteReceipt {
    std::string request_id;
    std::string transaction_id;
    std::uint32_t affected_rows = 0;
    std::uint64_t latency_us = 0;
    bool committed = false;
    bool replayed = false;
};

/** 表示 SQLite 适配器当前的健康状态。 */
enum class StorageHealthState { kUnavailable, kReady, kDegraded };

/** 汇总数据库架构、空间和提交延迟健康指标。 */
struct StorageHealth {
    StorageHealthState state = StorageHealthState::kUnavailable;
    std::uint64_t schema_revision = 0;
    std::uint64_t free_bytes = 0;
    std::uint64_t commit_count = 0;
    std::uint64_t max_commit_latency_us = 0;
};

/** 为领域 Store Port 提供统一读写和健康检查协议。 */
class StorageTransactionPort {
   public:
    /** @brief 允许通过接口类型释放存储事务端口。 */
    virtual ~StorageTransactionPort() = default;
    /**
     * @brief 执行一个命名只读请求。
     * @param request 待读取的协议请求。
     * @return 查询结果或错误。
     */
    virtual Result<StorageReadResult> Read(const StorageReadRequest& request) const = 0;
    /**
     * @brief 原子提交一个命名写请求批次。
     * @param request 待提交的协议请求。
     * @return 提交回执或错误。
     */
    virtual Result<StorageWriteReceipt> Commit(const StorageWriteRequest& request) = 0;
    /**
     * @brief 返回存储适配器健康快照。
     * @return 健康指标或错误。
     */
    virtual Result<StorageHealth> Health() const = 0;
};

}  // namespace voicelife::storage_sqlite
