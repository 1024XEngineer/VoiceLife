#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "support/test_support.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

using voicelife::schedule::CreateScheduleCommand;
using voicelife::schedule::DateTime;
using voicelife::schedule::DeleteScheduleCommand;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::ScheduleId;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ScheduleStatusFilter;
using voicelife::schedule::UpdateScheduleCommand;
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
 * @brief 保存跨数据库重连验证所需的 CRUD 结果标识。
 */
struct CrudResultIds {
    /** @brief 修改后应继续存在的日程标识。 */
    ScheduleId updated_schedule_id = 0;
    /** @brief 软删除后应保持取消状态的日程标识。 */
    ScheduleId cancelled_schedule_id = 0;
};

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
 * @brief 通过日程服务验证 SQLite 建表、创建、查询、修改和删除链路。
 * @param path 临时数据库路径。
 * @return 修改后保留及删除的日程标识。
 */
CrudResultIds CheckCrudThroughService(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "应成功打开真实 SQLite 数据库文件");
    CheckTransactionLifecycle(database);
    CheckStatementRowIdIsolation(database);
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "应成功创建日程表");
    ScheduleService service(repository, repository);

    Check(database.BeginTransaction().ok(), "Database 层应成功开始事务");
    const auto created = service.create_schedule(CreateScheduleCommand{
        .event = "SQLite 连接验证",
        .start_time = DateTime{std::chrono::seconds{2'000'000'000}},
        .end_time = DateTime{std::chrono::seconds{2'000'003'600}},
        .location = "会议室 A",
        .notes = "由真实仓储写入",
        .ignore_conflict = false,
    });
    Check(created.status.ok() && created.schedule.has_value() && created.schedule->id > 0,
          "服务应通过 SQLite 生成日程 ID");
    Check(database.Commit().ok(), "Database 层应成功提交事务");

    const auto second = service.create_schedule(CreateScheduleCommand{
        .event = "待删除日程",
        .start_time = DateTime{std::chrono::seconds{2'000'010'000}},
        .end_time = DateTime{std::chrono::seconds{2'000'010'600}},
        .location = std::nullopt,
        .notes = "删除后不应恢复",
        .ignore_conflict = false,
    });
    Check(second.status.ok() && second.schedule.has_value() && second.schedule->id > created.schedule->id,
          "第二条日程应通过 SQLite 获得独立标识");

    const auto queried = service.query_schedule({});
    Check(queried.status.ok() && queried.total == 2 && queried.schedules.size() == 2,
          "服务查询应读取两条刚写入的 SQLite 行");
    const auto first = service.query_schedule(QueryScheduleCommand{.schedule_id = created.schedule->id});
    Check(first.status.ok() && first.total == 1 && first.schedules.size() == 1, "按日程标识查询应命中真实 SQLite 行");
    const auto& stored = first.schedules.front();
    Check(stored.id == created.schedule->id && stored.event == "SQLite 连接验证" &&
              stored.start_time == DateTime{std::chrono::seconds{2'000'000'000}} &&
              stored.end_time == DateTime{std::chrono::seconds{2'000'003'600}} && stored.location == "会议室 A" &&
              stored.notes == "由真实仓储写入",
          "SQLite 查询应完整还原已写入的日程字段");

    UpdateScheduleCommand update;
    update.schedule_id = created.schedule->id;
    update.event = "  SQLite 修改验证  ";
    update.start_time = std::optional<DateTime>{DateTime{std::chrono::seconds{2'000'020'000}}};
    update.end_time = std::optional<DateTime>{DateTime{std::chrono::seconds{2'000'021'800}}};
    update.location = std::optional<std::string>{};
    update.notes = std::optional<std::string>{"修改后的真实备注"};
    const auto updated = service.update_schedule(update);
    Check(updated.status.ok() && updated.schedule.has_value() && updated.schedule->event == "SQLite 修改验证" &&
              !updated.schedule->location.has_value() && updated.schedule->notes == "修改后的真实备注",
          "服务修改应把全部字段及显式空值写入 SQLite");

    const auto deleted = service.delete_schedule(DeleteScheduleCommand{.schedule_id = second.schedule->id});
    Check(deleted.status.ok() && deleted.deleted, "服务删除应把 SQLite 日程标记为已取消");

    QueryScheduleCommand all;
    all.status = ScheduleStatusFilter::kAll;
    const auto after_changes = service.query_schedule(all);
    Check(after_changes.status.ok() && after_changes.total == 2 && after_changes.schedules.size() == 2,
          "修改和软删除后查询全部状态应保留两条历史日程");
    const auto cancelled = service.query_schedule(QueryScheduleCommand{
        .schedule_id = second.schedule->id,
        .keyword = std::nullopt,
        .start_from = std::nullopt,
        .start_to = std::nullopt,
        .status = ScheduleStatusFilter::kCancelled,
    });
    Check(cancelled.status.ok() && cancelled.total == 1 &&
              cancelled.schedules.front().status == ScheduleStatus::kCancelled,
          "软删除后的日程应通过取消状态查询命中");
    return {.updated_schedule_id = created.schedule->id, .cancelled_schedule_id = second.schedule->id};
}

/**
 * @brief 重新打开数据库并验证提交后的日程仍可查询。
 * @param path 已写入日程的数据库路径。
 * @param ids 要验证的修改及删除日程标识。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckRestartPersistence(const std::filesystem::path& path, const CrudResultIds& ids) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "重启场景应重新打开 SQLite 数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "重复初始化表结构应保持幂等");

    const auto stored = repository.FindAll();
    Check(stored.ok() && stored.value->size() == 2, "关闭并重连后应保留修改和软删除的日程");
    const auto updated_iter = std::find_if(stored.value->begin(), stored.value->end(), [&ids](const auto& schedule) {
        return schedule.id == ids.updated_schedule_id;
    });
    const auto cancelled_iter = std::find_if(stored.value->begin(), stored.value->end(), [&ids](const auto& schedule) {
        return schedule.id == ids.cancelled_schedule_id;
    });
    Check(updated_iter != stored.value->end() && cancelled_iter != stored.value->end() &&
              cancelled_iter->status == ScheduleStatus::kCancelled,
          "数据库重连后应保留软删除的取消状态");
    const auto& updated = *updated_iter;
    Check(updated.event == "SQLite 修改验证" && updated.start_time == DateTime{std::chrono::seconds{2'000'020'000}} &&
              updated.end_time == DateTime{std::chrono::seconds{2'000'021'800}} && !updated.location.has_value() &&
              updated.notes == "修改后的真实备注" && updated.rule_id == 88 &&
              updated.status == ScheduleStatus::kCompleted,
          "数据库重连后应完整保留更新字段");
}

}  // namespace

/**
 * @brief 执行真实 SQLite 日程仓储最小链路测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    const TemporaryDatabaseFile temporary = MakeTemporaryDatabaseFile();
    const CrudResultIds ids = CheckCrudThroughService(temporary.path);
    CheckRestartPersistence(temporary.path, ids);
    return 0;
}
