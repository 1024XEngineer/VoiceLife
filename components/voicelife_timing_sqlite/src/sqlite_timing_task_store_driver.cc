#include "voicelife/timing_sqlite/sqlite_timing_task_store_driver.h"

#include <utility>

namespace voicelife::timing_sqlite {
namespace {

const TimingStoreDriverDescriptor kSqliteDescriptor{
    .driver = "sqlite",
    .capabilities = {"atomic-timing-write", "restart-recovery", "event-outbox"},
    .resource_budget = {
        .max_open_connections = 1,
        .recommended_page_cache_bytes = 32 * 1024,
        .adapter_object_bytes = sizeof(SqliteTimingTaskStore),
    },
};

}  // namespace

const TimingStoreDriverDescriptor& SqliteTimingTaskStoreDriver::Descriptor() {
    return kSqliteDescriptor;
}

Status SqliteTimingTaskStoreDriver::ValidateConfig(const std::string& database_path) {
    if (database_path.empty() || database_path.size() > 255 ||
        database_path.find('\0') != std::string::npos) {
        return Status::Error(ErrorCode::kInvalidArgument, "SQLite database path is invalid");
    }
    return Status::Ok();
}

Result<std::unique_ptr<SqliteTimingTaskStore>> SqliteTimingTaskStoreDriver::Create(
    const std::string& database_path) {
    const Status valid = ValidateConfig(database_path);
    if (!valid.ok()) {
        return Result<std::unique_ptr<SqliteTimingTaskStore>>::Failure(valid.code, valid.message);
    }
    auto store = std::make_unique<SqliteTimingTaskStore>(database_path);
    const Status opened = store->Open();
    if (!opened.ok()) {
        return Result<std::unique_ptr<SqliteTimingTaskStore>>::Failure(opened.code, opened.message);
    }
    return Result<std::unique_ptr<SqliteTimingTaskStore>>::Success(std::move(store));
}

const TimingStoreDriverDescriptor* FindTimingStoreDriver(std::string_view driver) {
    return driver == kSqliteDescriptor.driver ? &kSqliteDescriptor : nullptr;
}

}  // namespace voicelife::timing_sqlite
