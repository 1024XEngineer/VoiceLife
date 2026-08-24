#include <chrono>
#include <filesystem>
#include <system_error>

#include "mapping/schedule_reminder_task_row_mapper.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_reminder_task_repository.h"
#include "voicelife/storage_sqlite/sqlite_database.h"
#include "voicelife/storage_sqlite/sqlite_schedule_reminder_task_repository.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::ScheduleReminderTask;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteScheduleReminderTaskRepository;
using voicelife::test::Check;

namespace {

/** 自动清理 SQLite 测试文件及其旁路文件。 */
struct TemporaryDatabase {
    std::filesystem::path path;

    /** @brief 清理测试数据库。 */
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
 * @return 自动清理的数据库文件描述。
 */
TemporaryDatabase MakeTemporaryDatabase() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return {.path = std::filesystem::temp_directory_path() /
                    ("voicelife-reminder-failure-" + std::to_string(suffix) + ".db")};
}

/**
 * @brief 创建字段合法的提醒任务。
 * @return 可用于仓储写入的提醒任务。
 */
ScheduleReminderTask MakeValidTask() {
    return {.id = 1,
            .schedule_id = 1,
            .chain_id = 1,
            .attempt = 1,
            .timing_task_id = "failure-path-reminder",
            .trigger_at = DateTime{std::chrono::seconds{2'000}},
            .triggered_at = std::nullopt,
            .created_at = DateTime{std::chrono::seconds{1'000}},
            .updated_at = DateTime{std::chrono::seconds{1'000}}};
}

/**
 * @brief 验证数据库未打开时全部提醒仓储操作都返回不可用。
 * @return 无。
 */
void CheckClosedDatabaseFailures() {
    SqliteDatabase database(":memory:");
    SqliteScheduleReminderTaskRepository repository(database);
    const ScheduleReminderTask task = MakeValidTask();

    Check(repository.Insert(task).status.code == ErrorCode::kUnavailable, "关闭数据库不应接受提醒写入");
    Check(repository.Update(task).code == ErrorCode::kUnavailable, "关闭数据库不应接受提醒更新");
    Check(repository.FindById(1).status.code == ErrorCode::kUnavailable, "关闭数据库不应执行标识查询");
    Check(repository.FindByTimingTaskId("timing").status.code == ErrorCode::kUnavailable,
          "关闭数据库不应执行 Timing task 标识查询");
    Check(repository.FindBySchedule(1).status.code == ErrorCode::kUnavailable, "关闭数据库不应执行日程查询");
    Check(repository.FindAll().status.code == ErrorCode::kUnavailable, "关闭数据库不应执行全量查询");
    Check(repository.FindTriggered(DateTime{}, DateTime{}).status.code == ErrorCode::kUnavailable,
          "关闭数据库不应执行触发查询");
}

/**
 * @brief 验证数据库缺少提醒表时全部 SQL 准备错误都会向上传递。
 * @return 无。
 */
void CheckMissingSchemaFailures() {
    const TemporaryDatabase temporary = MakeTemporaryDatabase();
    SqliteDatabase database(temporary.path.string());
    Check(database.Open().ok(), "缺失 Schema 测试应打开内存数据库");
    SqliteScheduleReminderTaskRepository repository(database);
    const ScheduleReminderTask task = MakeValidTask();

    Check(!repository.Insert(task).ok(), "缺少提醒表时插入应返回 SQL 准备错误");
    Check(!repository.Update(task).ok(), "缺少提醒表时更新应返回 SQL 准备错误");
    Check(!repository.FindById(1).ok(), "缺少提醒表时标识查询应返回 SQL 准备错误");
    Check(!repository.FindByTimingTaskId("timing").ok(), "缺少提醒表时 Timing task 标识查询应返回 SQL 准备错误");
    Check(!repository.FindBySchedule(1).ok(), "缺少提醒表时日程查询应返回 SQL 准备错误");
    Check(!repository.FindAll().ok(), "缺少提醒表时全量查询应返回 SQL 准备错误");
    Check(!repository.FindTriggered(DateTime{}, DateTime{}).ok(), "缺少提醒表时触发查询应返回 SQL 准备错误");
}

/**
 * @brief 验证行映射器拒绝数据库中的非法提醒状态。
 * @return 无。
 */
void CheckInvalidStoredStatus() {
    const TemporaryDatabase temporary = MakeTemporaryDatabase();
    SqliteDatabase database(temporary.path.string());
    Check(database.Open().ok(), "非法状态映射测试应打开内存数据库");
    auto prepared = database.Prepare("SELECT 1, 1, 1, 1, NULL, 2000, 99, 1, NULL, 1000, 1000;");
    Check(prepared.ok(), "非法状态映射测试应准备查询语句");
    auto statement = std::move(*prepared.value);
    const auto step = statement.Step();
    Check(step.ok(), "非法状态映射测试应读取一行数据");
    const auto mapped = voicelife::storage_sqlite::mapping::ReadScheduleReminderTask(statement);
    Check(!mapped.ok() && mapped.status.code == ErrorCode::kInternal, "非法业务状态应被行映射器拒绝");
}

}  // namespace

int main() {
    CheckClosedDatabaseFailures();
    CheckMissingSchemaFailures();
    CheckInvalidStoredStatus();
    return 0;
}
