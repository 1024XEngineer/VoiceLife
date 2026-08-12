#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "mapping/schedule_row_mapper.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_types.h"
#include "voicelife/storage_sqlite/sqlite_database.h"
#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleStatus;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteScheduleRepository;
using voicelife::storage_sqlite::SqliteStep;
using voicelife::test::Check;

namespace mapping = voicelife::storage_sqlite::mapping;

namespace {

/** @brief 管理 Repository 单元测试使用的临时数据库文件。 */
struct TemporaryDatabaseFile {
    std::filesystem::path path;

    /** @brief 删除数据库及其日志文件。 @return 无。 */
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
                    ("voicelife-repository-unit-" + std::to_string(suffix) + ".db")};
}

/** @brief 将测试 Unix 秒转换为日程时间。 @param seconds Unix 秒。 @return 日程时间。 */
DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/**
 * @brief 创建包含全部可选字段的日程。
 * @return 完整测试日程。
 */
Schedule CompleteSchedule() {
    return {
        .id = 55,
        .event = "完整字段日程",
        .start_time = At(2'100'000'000),
        .end_time = At(2'100'003'600),
        .location = "会议室 C",
        .notes = "完整字段往返",
        .rule_id = 88,
        .status = ScheduleStatus::kCancelled,
        .created_at = At(2'000'000'000),
        .updated_at = At(2'000'000'100),
    };
}

/**
 * @brief 验证数据库未打开时 Repository 返回不可用。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckUnavailableRepository(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().code == ErrorCode::kUnavailable, "未打开数据库不能初始化 Repository");
    Check(repository.Insert(CompleteSchedule()).status.code == ErrorCode::kUnavailable, "未打开数据库不能写入日程");
    Check(repository.FindAll().status.code == ErrorCode::kUnavailable, "未打开数据库不能查询日程");
}

/**
 * @brief 验证空标题、默认时间和所有字段往返。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckInsertAndRoundTrip(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "Repository 测试应打开数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok() && repository.Initialize().ok(), "Repository 初始化应保持幂等");
    Check(repository.Insert(Schedule{}).status.code == ErrorCode::kInvalidArgument, "空标题日程应被拒绝");

    const auto minimal = repository.Insert(Schedule{
        .id = 999,
        .event = "最小日程",
        .start_time = std::nullopt,
        .end_time = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .rule_id = std::nullopt,
        .status = ScheduleStatus::kActive,
        .created_at = {},
        .updated_at = {},
    });
    Check(minimal.ok() && minimal.value->id > 0 && minimal.value->id != 999, "Repository 应生成标识并忽略调用方标识");
    Check(minimal.value->created_at != DateTime{} && minimal.value->updated_at == minimal.value->created_at,
          "Repository 应为最小日程补齐时间戳");

    const Schedule complete_input = CompleteSchedule();
    const auto complete = repository.Insert(complete_input);
    Check(complete.ok() && complete.value->created_at == complete_input.created_at &&
              complete.value->updated_at == complete_input.updated_at,
          "Repository 应保留调用方提供的时间戳");

    const auto stored = repository.FindAll();
    Check(stored.ok() && stored.value->size() == 2, "Repository 应读取两条日程");
    const Schedule& complete_row = stored.value->front();
    Check(complete_row.event == complete_input.event && complete_row.start_time == complete_input.start_time &&
              complete_row.end_time == complete_input.end_time && complete_row.location == complete_input.location &&
              complete_row.notes == complete_input.notes && complete_row.rule_id == complete_input.rule_id &&
              complete_row.status == complete_input.status && complete_row.created_at == complete_input.created_at &&
              complete_row.updated_at == complete_input.updated_at,
          "完整日程的所有字段都应往返一致");
    const Schedule& minimal_row = stored.value->back();
    Check(!minimal_row.start_time.has_value() && !minimal_row.end_time.has_value() &&
              !minimal_row.location.has_value() && !minimal_row.notes.has_value() && !minimal_row.rule_id.has_value(),
          "最小日程的可空字段应保持为空");
}

/**
 * @brief 验证 Mapper 拒绝非法状态和空标题结果行。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckMapperValidation(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "Mapper 测试应打开数据库");

    auto invalid_status = database.Prepare("SELECT 1, '日程', NULL, NULL, NULL, NULL, NULL, 99, 100, 100");
    Check(invalid_status.ok() && invalid_status.value->Step().ok(), "应构造非法状态结果行");
    Check(mapping::ReadSchedule(*invalid_status.value).status.code == ErrorCode::kInternal,
          "Mapper 应拒绝非法日程状态");

    auto null_event = database.Prepare("SELECT 1, NULL, NULL, NULL, NULL, NULL, NULL, 1, 100, 100");
    Check(null_event.ok() && null_event.value->Step().ok(), "应构造空标题结果行");
    Check(mapping::ReadSchedule(*null_event.value).status.code == ErrorCode::kInternal, "Mapper 应拒绝空标题结果行");

    auto no_parameters = database.Prepare("SELECT 1");
    Check(no_parameters.ok(), "应创建无参数语句");
    const auto event_error = mapping::BindSchedule(*no_parameters.value, CompleteSchedule());
    Check(event_error.code == ErrorCode::kInternal && event_error.message.find("event") != std::string::npos,
          "Mapper 应为标题绑定错误补充字段名");

    auto one_parameter = database.Prepare("SELECT ?");
    Check(one_parameter.ok(), "应创建单参数语句");
    const auto start_error = mapping::BindSchedule(*one_parameter.value, CompleteSchedule());
    Check(start_error.code == ErrorCode::kInternal && start_error.message.find("start_time") != std::string::npos,
          "Mapper 应为开始时间绑定错误补充字段名");
}

/**
 * @brief 验证 Repository 会传播 SQL 编译、执行和行映射错误。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckRepositoryErrorPropagation(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "错误传播测试应打开数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "错误传播测试应初始化表");

    Check(database
              .Execute("CREATE TRIGGER reject_schedule BEFORE INSERT ON schedule "
                       "BEGIN SELECT RAISE(ABORT, 'blocked'); END")
              .ok(),
          "应成功创建拒绝写入触发器");
    Check(repository.Insert(CompleteSchedule()).status.code == ErrorCode::kAlreadyExists,
          "Repository 应传播 Statement 执行错误");
    Check(database.Execute("DROP TRIGGER reject_schedule").ok(), "应删除拒绝写入触发器");

    Check(database.Execute("DROP TABLE schedule").ok(), "应删除日程表以制造 SQL 编译错误");
    Check(repository.Insert(CompleteSchedule()).status.code == ErrorCode::kInternal,
          "Repository 应传播写入 SQL 编译错误");
    Check(repository.FindAll().status.code == ErrorCode::kInternal, "Repository 应传播查询 SQL 编译错误");
}

}  // namespace

/** @brief 执行 SQLite 日程 Repository 和 Mapper 单元测试。 @return 全部断言通过时返回 0。 */
int main() {
    const TemporaryDatabaseFile unavailable = MakeTemporaryDatabaseFile();
    CheckUnavailableRepository(unavailable.path);
    const TemporaryDatabaseFile round_trip = MakeTemporaryDatabaseFile();
    CheckInsertAndRoundTrip(round_trip.path);
    const TemporaryDatabaseFile mapper = MakeTemporaryDatabaseFile();
    CheckMapperValidation(mapper.path);
    const TemporaryDatabaseFile errors = MakeTemporaryDatabaseFile();
    CheckRepositoryErrorPropagation(errors.path);
    return 0;
}
