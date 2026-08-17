#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "support/test_support.h"
#include "voicelife/schedule/schedule_types.h"
#include "voicelife/storage_sqlite/sqlite_database.h"
#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::OperationRecord;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleOperationType;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteScheduleRepository;
using voicelife::test::Check;

namespace {

/** @brief 管理新增 SQLite 回滚覆盖测试使用的临时数据库文件。 */
struct TemporaryDatabaseFile {
    std::filesystem::path path;

    /** @brief 删除数据库及关联日志文件。 @return 无。 */
    ~TemporaryDatabaseFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-journal", error);
        std::filesystem::remove(path.string() + "-wal", error);
        std::filesystem::remove(path.string() + "-shm", error);
    }
};

/** @brief 创建唯一临时数据库路径。 @return 尚不存在的 SQLite 文件路径。 */
TemporaryDatabaseFile MakeTemporaryDatabaseFile() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return {.path = std::filesystem::temp_directory_path() /
                    ("voicelife-rollback-coverage-" + std::to_string(suffix) + ".db")};
}

/** @brief 返回当前秒级系统时间。 @return 当前日程时间。 */
DateTime CurrentTime() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

/** @brief 将测试 Unix 秒转换为日程时间。 @param seconds Unix 秒。 @return 日程时间。 */
DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/** @brief 构造仅含事件名的日程。 @param event 日程名称。 @return 最小日程。 */
Schedule MinimalSchedule(const std::string& event) {
    Schedule schedule;
    schedule.event = event;
    return schedule;
}

/**
 * @brief 验证操作表缺失时撤销操作读取 SQL 失败并回滚。
 * @return 无。
 */
void CheckMissingOperationTableRollback() {
    const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
    SqliteDatabase database(file.path.string());
    Check(database.Open().ok(), "回滚覆盖测试应打开数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "回滚覆盖测试应初始化表结构");
    Check(database.Execute("DROP TABLE operation_record").ok(), "应删除操作表制造读取操作记录 SQL 错误");
    Check(repository.UndoOperation(1, CurrentTime()).status.code == ErrorCode::kInternal,
          "撤销读取操作记录失败应透传内部错误");
}

/**
 * @brief 验证撤销创建操作时读取目标日程失败并回滚。
 * @return 无。
 */
void CheckMissingScheduleTableRollback() {
    const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
    SqliteDatabase database(file.path.string());
    Check(database.Open().ok(), "日程表回滚覆盖测试应打开数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "日程表回滚覆盖测试应初始化表结构");

    Schedule base = MinimalSchedule("撤销目标日程");
    base.start_time = At(2'100'000'000);
    base.end_time = At(2'100'003'600);
    const auto target = repository.Insert(base);
    Check(target.ok(), "应创建撤销目标日程");

    OperationRecord operation;
    operation.type = ScheduleOperationType::kCreate;
    operation.schedule_id = target.value->id;
    operation.schedule_event = "撤销创建";
    const auto saved = repository.InsertOperation(operation);
    Check(saved.ok(), "应保存创建操作");

    Check(database.Execute("DROP TABLE schedule").ok(), "应删除日程表制造读取目标日程 SQL 错误");
    Check(repository.UndoOperation(saved.value->id, CurrentTime()).status.code == ErrorCode::kInternal,
          "撤销读取目标日程失败应透传内部错误");
}

}  // namespace

/**
 * @brief 执行新增的 SQLite 日程仓库撤销事务回滚覆盖测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    CheckMissingOperationTableRollback();
    CheckMissingScheduleTableRollback();
    return 0;
}
