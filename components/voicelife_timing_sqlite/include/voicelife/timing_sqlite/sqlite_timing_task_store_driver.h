#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "voicelife/timing_sqlite/sqlite_timing_task_store.h"

namespace voicelife::timing_sqlite {

struct TimingStoreResourceBudget {
    unsigned max_open_connections = 1;
    unsigned recommended_page_cache_bytes = 32 * 1024;
    unsigned adapter_object_bytes = 0;
};

struct TimingStoreDriverDescriptor {
    std::string_view driver;
    std::array<std::string_view, 3> capabilities;
    TimingStoreResourceBudget resource_budget;
};

class SqliteTimingTaskStoreDriver final {
   public:
    static const TimingStoreDriverDescriptor& Descriptor();
    static Status ValidateConfig(const std::string& database_path);
    static Result<std::unique_ptr<SqliteTimingTaskStore>> Create(const std::string& database_path);
};

// Compile-time registry for timing storage adapters linked into this component.
const TimingStoreDriverDescriptor* FindTimingStoreDriver(std::string_view driver);

}  // namespace voicelife::timing_sqlite
