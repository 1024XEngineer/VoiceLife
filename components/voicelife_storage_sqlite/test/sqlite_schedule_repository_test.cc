#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "support/test_support.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

using voicelife::schedule::CreateScheduleCommand;
using voicelife::schedule::ScheduleService;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteScheduleRepository;
using voicelife::test::Check;

namespace {

/** 管理集成测试使用的唯一临时数据库文件。 */
struct TemporaryDatabaseFile {
    std::filesystem::path path;

    /**
     * @brief 删除测试产生的数据库及其附属日志文件。
     * @return 无返回值。
     */
    ~TemporaryDatabaseFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-journal", error);
        std::filesystem::remove(path.string() + "-wal", error);
        std::filesystem::remove(path.string() + "-shm", error);
    }
};

/**
 * @brief 创建本测试进程专用的临时数据库路径。
 * @return 尚不存在的 SQLite 文件路径。
 */
TemporaryDatabaseFile MakeTemporaryDatabaseFile() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return {.path = std::filesystem::temp_directory_path() / ("voicelife-schedule-" + std::to_string(suffix) + ".db")};
}

/**
 * @brief 验证 Statement 存活期间仍可结束事务，防止 Database 连接锁自锁。
 * @param database 已打开的 SQLite 数据库。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckTransactionLifecycle(SqliteDatabase& database) {
    Check(database.BeginTransaction().ok(), "应成功开始基础设施事务");
    const auto prepared = database.Prepare("SELECT 1");
    Check(prepared.ok(), "事务内应成功创建 Statement");
    Check(database.Rollback().ok(), "Statement 存活期间回滚不应发生连接锁自锁");
}

/**
 * @brief 验证交错写入不会覆盖各 Statement 缓存的自增标识。
 * @param database 已打开的 SQLite 数据库。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckStatementRowIdIsolation(SqliteDatabase& database) {
    Check(database.Execute("CREATE TABLE rowid_probe (id INTEGER PRIMARY KEY AUTOINCREMENT)").ok(),
          "应成功创建 Statement 行号隔离测试表");
    auto first = database.Prepare("INSERT INTO rowid_probe DEFAULT VALUES");
    auto second = database.Prepare("INSERT INTO rowid_probe DEFAULT VALUES");
    Check(first.ok() && second.ok(), "应成功创建两个写入 Statement");
    Check(first.value->Step().ok() && second.value->Step().ok(), "两个交错写入都应执行成功");
    Check(first.value->LastInsertRowId() == 1 && second.value->LastInsertRowId() == 2,
          "每个 Statement 应保留自身写入产生的行号");
}

/**
 * @brief 通过日程服务验证 SQLite 建表、写入和查询链路。
 * @param path 临时数据库路径。
 * @return 成功创建的日程标识。
 */
int64_t CheckWriteAndQueryThroughService(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "应成功打开真实 SQLite 数据库文件");
    CheckTransactionLifecycle(database);
    CheckStatementRowIdIsolation(database);
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "应成功创建日程表");
    ScheduleService service(repository);

    Check(database.BeginTransaction().ok(), "Database 层应成功开始事务");
    const auto created = service.create_schedule(CreateScheduleCommand{
        .event = "SQLite 连接验证",
        .start_time = voicelife::schedule::DateTime{std::chrono::seconds{2'000'000'000}},
        .end_time = voicelife::schedule::DateTime{std::chrono::seconds{2'000'003'600}},
        .location = "会议室 A",
        .notes = "由真实仓储写入",
        .ignore_conflict = false,
    });
    Check(created.status.ok() && created.schedule.has_value() && created.schedule->id > 0,
          "服务应通过 SQLite 生成日程 ID");
    Check(database.Commit().ok(), "Database 层应成功提交事务");

    const auto queried = service.query_schedule({});
    Check(queried.status.ok() && queried.total == 1 && queried.schedules.size() == 1,
          "服务查询应读取刚写入的 SQLite 行");
    const auto& stored = queried.schedules.front();
    Check(stored.id == created.schedule->id && stored.event == "SQLite 连接验证" &&
              stored.start_time == voicelife::schedule::DateTime{std::chrono::seconds{2'000'000'000}} &&
              stored.end_time == voicelife::schedule::DateTime{std::chrono::seconds{2'000'003'600}} &&
              stored.location == "会议室 A" && stored.notes == "由真实仓储写入",
          "SQLite 查询应完整还原已写入的日程字段");
    return created.schedule->id;
}

/**
 * @brief 重新打开数据库并验证提交后的日程仍可查询。
 * @param path 已写入日程的数据库路径。
 * @param schedule_id 要验证的日程标识。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckRestartPersistence(const std::filesystem::path& path, int64_t schedule_id) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "重启场景应重新打开 SQLite 数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "重复初始化表结构应保持幂等");

    const auto stored = repository.FindAll();
    Check(stored.ok() && stored.value->size() == 1 && stored.value->front().id == schedule_id,
          "关闭并重连后应保留已写入的日程");
}

}  // namespace

/**
 * @brief 执行真实 SQLite 日程仓储最小链路测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    const TemporaryDatabaseFile temporary = MakeTemporaryDatabaseFile();
    const int64_t schedule_id = CheckWriteAndQueryThroughService(temporary.path);
    CheckRestartPersistence(temporary.path, schedule_id);
    return 0;
}
