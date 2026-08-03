#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "voicelife/contracts/status.h"

namespace voicelife::storage_sqlite {

// Named statements are owned by a domain SQLite adapter. The shared substrate
// never accepts raw SQL or table names, which keeps schema details out of the
// application and the real-time voice path.
using StorageValue = std::variant<std::monostate, std::int64_t, double, bool, std::string,
                                  std::vector<std::uint8_t>>;

struct StorageStatement {
    std::string name;
    std::vector<StorageValue> arguments;

    [[nodiscard]] Status Validate() const;
};

struct StorageRequestContext {
    std::string request_id;
    std::uint32_t deadline_ms = 0;

    [[nodiscard]] Status Validate() const;
};

struct StorageReadRequest {
    StorageRequestContext context;
    StorageStatement query;

    [[nodiscard]] Status Validate() const;
};

struct StorageWriteRequest {
    StorageRequestContext context;
    std::vector<StorageStatement> statements;

    [[nodiscard]] Status Validate() const;
};

struct StorageRow {
    std::vector<StorageValue> values;
};

struct StorageReadResult {
    std::string request_id;
    std::vector<StorageRow> rows;
    std::uint64_t snapshot_revision = 0;
};

struct StorageWriteReceipt {
    std::string request_id;
    std::string transaction_id;
    std::uint32_t affected_rows = 0;
    std::uint64_t latency_us = 0;
    bool committed = false;
    bool replayed = false;
};

enum class StorageHealthState { kUnavailable, kReady, kDegraded };

struct StorageHealth {
    StorageHealthState state = StorageHealthState::kUnavailable;
    std::uint64_t schema_revision = 0;
    std::uint64_t free_bytes = 0;
    std::uint64_t commit_count = 0;
    std::uint64_t max_commit_latency_us = 0;
};

// This is the only shared read/write protocol. Domain modules keep their own
// semantic Store Port and map it to named statements inside their SQLite
// adapter. The adapter owns one connection, migrations, PRAGMAs and the
// bounded single-writer queue.
class StorageTransactionPort {
   public:
    virtual ~StorageTransactionPort() = default;
    virtual Result<StorageReadResult> Read(const StorageReadRequest& request) const = 0;
    virtual Result<StorageWriteReceipt> Commit(const StorageWriteRequest& request) = 0;
    virtual Result<StorageHealth> Health() const = 0;
};

}  // namespace voicelife::storage_sqlite
