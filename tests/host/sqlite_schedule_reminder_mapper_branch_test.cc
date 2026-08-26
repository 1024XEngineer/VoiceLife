#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#include "mapping/schedule_reminder_task_row_mapper.h"
#include "support/test_support.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

using voicelife::ErrorCode;
using voicelife::schedule::ScheduleReminderTask;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::test::Check;

namespace {

/** 自动清理 Mapper 测试使用的 SQLite 文件。 */
struct TemporaryDatabase {
    std::filesystem::path path;

    /** @brief 清理数据库及旁路文件。 */
    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-journal", error);
        std::filesystem::remove(path.string() + "-wal", error);
        std::filesystem::remove(path.string() + "-shm", error);
    }
};

/**
 * @brief 创建唯一的临时数据库路径。
 * @return 自动清理的数据库描述。
 */
TemporaryDatabase MakeTemporaryDatabase() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return {.path = std::filesystem::temp_directory_path() /
                    ("voicelife-reminder-mapper-" + std::to_string(suffix) + ".db")};
}

/**
 * @brief 验证提醒任务绑定器会透传可选字段绑定失败。
 * @return 无。
 */
void CheckOptionalFieldBindingFailures() {
    const TemporaryDatabase temporary = MakeTemporaryDatabase();
    SqliteDatabase database(temporary.path.string());
    Check(database.Open().ok(), "Mapper 分支测试应打开临时数据库");
    ScheduleReminderTask task;
    task.schedule_id = 1;
    task.chain_id = 1;
    task.attempt = 1;
    task.timing_task_id = "timing-task";
    task.trigger_at = voicelife::schedule::DateTime{std::chrono::seconds{2'000}};
    task.triggered_at = voicelife::schedule::DateTime{std::chrono::seconds{2'001}};

    auto timing_prepared = database.Prepare("SELECT ?1, ?2, ?3;");
    Check(timing_prepared.ok(), "Timing 标识绑定失败测试应准备三参数语句");
    auto timing_statement = std::move(*timing_prepared.value);
    Check(!voicelife::storage_sqlite::mapping::BindScheduleReminderTask(timing_statement, task).ok(),
          "第四个 Timing 标识参数缺失时绑定器应返回错误");

    auto triggered_prepared = database.Prepare("SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7;");
    Check(triggered_prepared.ok(), "触发时间绑定失败测试应准备七参数语句");
    auto triggered_statement = std::move(*triggered_prepared.value);
    Check(!voicelife::storage_sqlite::mapping::BindScheduleReminderTask(triggered_statement, task).ok(),
          "第八个触发时间参数缺失时绑定器应返回错误");
}

/**
 * @brief 验证提醒任务行映射器拒绝非法 Timer 状态。
 * @return 无。
 */
void CheckInvalidTimerStatus() {
    const TemporaryDatabase temporary = MakeTemporaryDatabase();
    SqliteDatabase database(temporary.path.string());
    Check(database.Open().ok(), "非法 Timer 状态测试应打开临时数据库");
    auto prepared = database.Prepare("SELECT 1, 1, 1, 1, NULL, 2000, 1, 99, NULL, 1000, 1000;");
    Check(prepared.ok(), "非法 Timer 状态测试应准备查询语句");
    auto statement = std::move(*prepared.value);
    Check(statement.Step().ok(), "非法 Timer 状态测试应读取一行");
    const auto mapped = voicelife::storage_sqlite::mapping::ReadScheduleReminderTask(statement);
    Check(!mapped.ok() && mapped.status.code == ErrorCode::kInternal, "非法 Timer 状态应被行映射器拒绝");
}

/**
 * @brief 验证提醒任务行映射器拒绝非法动作类型。
 * @return 无。
 */
void CheckInvalidActionKind() {
    const TemporaryDatabase temporary = MakeTemporaryDatabase();
    SqliteDatabase database(temporary.path.string());
    Check(database.Open().ok(), "非法动作类型测试应打开临时数据库");
    auto prepared =
        database.Prepare("SELECT 1, 1, 1, 1, 'timing', 2000, 6, 2, 2000, 'operation', 99, 2001, 2600, 1000, 2001;");
    Check(prepared.ok(), "非法动作类型测试应准备查询语句");
    auto statement = std::move(*prepared.value);
    Check(statement.Step().ok(), "非法动作类型测试应读取一行");
    const auto mapped = voicelife::storage_sqlite::mapping::ReadScheduleReminderTask(statement);
    Check(!mapped.ok() && mapped.status.code == ErrorCode::kInternal, "非法动作类型应被行映射器拒绝");
}

}  // namespace

int main() {
    CheckOptionalFieldBindingFailures();
    CheckInvalidTimerStatus();
    CheckInvalidActionKind();
    return 0;
}
