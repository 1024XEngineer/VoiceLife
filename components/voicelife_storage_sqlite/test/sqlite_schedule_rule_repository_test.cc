#include "voicelife/storage_sqlite/sqlite_schedule_rule_repository.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "mapping/schedule_exception_row_mapper.h"
#include "mapping/schedule_rule_row_mapper.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_types.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::ExceptionType;
using voicelife::schedule::Frequency;
using voicelife::schedule::LocalDate;
using voicelife::schedule::LocalTime;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleException;
using voicelife::schedule::ScheduleRule;
using voicelife::schedule::ScheduleRuleId;
using voicelife::schedule::ScheduleStatus;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteScheduleRuleRepository;
using voicelife::test::Check;

namespace mapping = voicelife::storage_sqlite::mapping;

namespace {

/** @brief 管理测试进程专用的临时数据库文件。 */
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

/** @brief 生成临时数据库路径。 @return 不存在的 SQLite 文件路径。 */
TemporaryDatabaseFile MakeTemporaryDatabaseFile() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return {.path = std::filesystem::temp_directory_path() / ("voicelife-rule-" + std::to_string(suffix) + ".db")};
}

/** @brief 构造用于测试的完整周期规则。 @return 每日 09:00 规则。 */
ScheduleRule DailyRule() {
    ScheduleRule rule;
    rule.event = "每日例会";
    rule.location = "会议室";
    rule.notes = "复盘";
    rule.freq_type = Frequency::kDaily;
    rule.interval_val = 1;
    rule.start_time = LocalTime{9, 0, 0};
    rule.start_date = LocalDate{2099, 1, 1};
    rule.status = ScheduleStatus::kActive;
    return rule;
}

/** @brief 构造包含全部可空字段和年结束日期的规则。 @return 完整月规则。 */
ScheduleRule FullRuleWithEndDate() {
    ScheduleRule rule = DailyRule();
    rule.event = "完整月规则";
    rule.freq_type = Frequency::kMonthly;
    rule.interval_val = 2;
    rule.weekdays_mask = uint8_t{1};
    rule.day_of_month = uint8_t{15};
    rule.month_of_year = uint8_t{6};
    rule.monthly_mode = voicelife::schedule::MonthlyMode::kSpecificDay;
    rule.start_time = LocalTime{8, 30, 15};
    rule.end_time = LocalTime{9, 15, 45};
    rule.start_date = LocalDate{2099, 6, 15};
    rule.end_date = LocalDate{2099, 12, 31};
    return rule;
}

/** @brief 构造包含发生次数且无结束日期的规则。 @return 完整次数规则。 */
ScheduleRule FullRuleWithCount() {
    ScheduleRule rule = FullRuleWithEndDate();
    rule.event = "完整次数规则";
    rule.monthly_mode = voicelife::schedule::MonthlyMode::kLastDay;
    rule.day_of_month = std::nullopt;
    rule.end_date = std::nullopt;
    rule.occurrence_count = 8;
    return rule;
}

/** @brief 构造待物化的首条实例。 @param rule_id 规则标识。 @return 日程实例。 */
Schedule FirstInstance(ScheduleRuleId rule_id) {
    Schedule schedule;
    schedule.event = "每日例会";
    schedule.start_time = DateTime{std::chrono::seconds{4'071'171'600}};
    schedule.end_time = DateTime{std::chrono::seconds{4'071'175'200}};
    schedule.location = "会议室";
    schedule.notes = "复盘";
    schedule.rule_id = rule_id;
    return schedule;
}

/** @brief 构造可复用的单次例外。 @param rule_id 规则标识。 @return 修改类型例外。 */
ScheduleException ModifyException(ScheduleRuleId rule_id) {
    ScheduleException exception;
    exception.rule_id = rule_id;
    exception.original_start_time = DateTime{std::chrono::seconds{4'071'258'000}};
    exception.type = ExceptionType::kModify;
    return exception;
}

/**
 * @brief 验证规则与例外 Mapper 的绑定错误和非法结果行拒绝分支。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckRuleMapperValidation(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "规则 Mapper 测试应打开数据库");

    auto no_parameters = database.Prepare("SELECT 1");
    Check(no_parameters.ok(), "应创建无参数语句");
    const auto rule_bind = mapping::BindScheduleRule(*no_parameters.value, DailyRule());
    Check(rule_bind.code == ErrorCode::kInternal && rule_bind.message.find("event") != std::string::npos,
          "规则 Mapper 应为 event 绑定错误补充字段名");

    ScheduleException exception;
    exception.rule_id = 1;
    exception.original_start_time = DateTime{std::chrono::seconds{4'071'258'000}};
    exception.type = ExceptionType::kModify;
    const auto exception_bind = mapping::BindScheduleException(*no_parameters.value, exception);
    Check(exception_bind.code == ErrorCode::kInternal && exception_bind.message.find("rule_id") != std::string::npos,
          "例外 Mapper 应为 rule_id 绑定错误补充字段名");

    auto bad_freq = database.Prepare(
        "SELECT 1, '规则', NULL, NULL, 99, 1, NULL, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, 1, 0, 0");
    Check(bad_freq.ok() && bad_freq.value->Step().ok(), "应构造非法频率结果行");
    Check(mapping::ReadScheduleRule(*bad_freq.value).status.code == ErrorCode::kInternal, "规则 Mapper 应拒绝非法频率");

    auto bad_status = database.Prepare(
        "SELECT 1, '规则', NULL, NULL, 1, 1, NULL, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, 99, 0, 0");
    Check(bad_status.ok() && bad_status.value->Step().ok(), "应构造非法状态结果行");
    Check(mapping::ReadScheduleRule(*bad_status.value).status.code == ErrorCode::kInternal,
          "规则 Mapper 应拒绝非法状态");

    auto null_name =
        database.Prepare("SELECT 1, NULL, NULL, NULL, 1, 1, NULL, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, 1, 0, 0");
    Check(null_name.ok() && null_name.value->Step().ok(), "应构造空名称结果行");
    Check(mapping::ReadScheduleRule(*null_name.value).status.code == ErrorCode::kInternal, "规则 Mapper 应拒绝空名称");

    auto bad_mode =
        database.Prepare("SELECT 1, '规则', NULL, NULL, 1, 1, NULL, NULL, NULL, 99, 0, NULL, 0, NULL, NULL, 1, 0, 0");
    Check(bad_mode.ok() && bad_mode.value->Step().ok(), "应构造非法月模式结果行");
    Check(mapping::ReadScheduleRule(*bad_mode.value).status.code == ErrorCode::kInternal,
          "规则 Mapper 应拒绝非法月模式");

    auto bad_type = database.Prepare("SELECT 1, 1, 0, NULL, 99, NULL, NULL, NULL, NULL, NULL, 0, 0");
    Check(bad_type.ok() && bad_type.value->Step().ok(), "应构造非法例外类型结果行");
    Check(mapping::ReadScheduleException(*bad_type.value).status.code == ErrorCode::kInternal,
          "例外 Mapper 应拒绝非法类型");

    auto override_times = database.Prepare(
        "SELECT 1, 1, 4071258000, NULL, 1, 4071258000, 4071261600, '改标题', NULL, NULL, 2000000000, 2000000100");
    Check(override_times.ok() && override_times.value->Step().ok(), "应构造带覆盖时间的例外结果行");
    const auto read_override = mapping::ReadScheduleException(*override_times.value);
    Check(read_override.ok() && read_override.value->override_start_time.has_value() &&
              read_override.value->override_end_time.has_value(),
          "例外 Mapper 应还原非空覆盖时间");
}

/**
 * @brief 验证规则 Mapper 能完整往返全部可空字段的两种合法组合。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckFullRuleRoundTrip(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "完整规则测试应打开数据库");
    SqliteScheduleRuleRepository repository(database);
    Check(repository.Initialize().ok(), "完整规则测试应初始化表结构");

    const auto with_end_date = repository.Insert(FullRuleWithEndDate());
    Check(with_end_date.ok() && with_end_date.value->id > 0, "带结束日期的完整规则应插入成功");
    const auto loaded_end_date = repository.FindById(with_end_date.value->id);
    Check(loaded_end_date.ok() && loaded_end_date.value->location == "会议室" &&
              loaded_end_date.value->notes == "复盘" && loaded_end_date.value->weekdays_mask == uint8_t{1} &&
              loaded_end_date.value->day_of_month == uint8_t{15} &&
              loaded_end_date.value->month_of_year == uint8_t{6} &&
              loaded_end_date.value->monthly_mode == voicelife::schedule::MonthlyMode::kSpecificDay &&
              loaded_end_date.value->end_time.has_value() && loaded_end_date.value->end_time->hour == 9 &&
              loaded_end_date.value->end_time->minute == 15 && loaded_end_date.value->end_time->second == 45 &&
              loaded_end_date.value->end_date.has_value() && loaded_end_date.value->end_date->year == 2099 &&
              loaded_end_date.value->end_date->month == 12 && loaded_end_date.value->end_date->day == 31,
          "带结束日期的完整规则应还原全部可空字段");

    const auto with_count = repository.Insert(FullRuleWithCount());
    Check(with_count.ok() && with_count.value->id > 0, "带发生次数的完整规则应插入成功");
    const auto loaded_count = repository.FindById(with_count.value->id);
    Check(loaded_count.ok() && !loaded_count.value->day_of_month.has_value() &&
              loaded_count.value->monthly_mode == voicelife::schedule::MonthlyMode::kLastDay &&
              !loaded_count.value->end_date.has_value() && loaded_count.value->occurrence_count == 8,
          "带发生次数的完整规则应还原空结束日期和次数");
}

/**
 * @brief 验证规则仓储的空字段、无首条实例和非法标识等分支。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckRuleRepositoryBranches(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "规则分支测试应打开数据库");
    SqliteScheduleRuleRepository repository(database);
    Check(repository.Initialize().ok(), "规则分支测试应初始化表结构");

    ScheduleRule empty = DailyRule();
    empty.event = "";
    Check(repository.Insert(empty).status.code == ErrorCode::kInvalidArgument, "空规则名 Insert 应被拒绝");
    Check(repository.CreateWithFirstInstance(empty, FirstInstance(0)).status.code == ErrorCode::kInvalidArgument,
          "空规则名 CreateWithFirstInstance 应被拒绝");

    const auto rule_no_first = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
    Check(rule_no_first.ok() && rule_no_first.value->id > 0, "无首条实例的创建应成功");
    const ScheduleRuleId rule_id = rule_no_first.value->id;

    ScheduleRule updated = *rule_no_first.value;
    updated.event = "无实例更新";
    Check(repository.UpdateAndRebuild(updated, std::nullopt).ok(), "无首条实例的更新应成功");

    // Insert / Update 单独调用（不涉及实例重建）。
    ScheduleRule inserted = DailyRule();
    inserted.event = "单独插入";
    const auto insert_result = repository.Insert(inserted);
    Check(insert_result.ok() && insert_result.value->id > 0, "Insert 应成功插入规则");

    ScheduleRule direct_update = *rule_no_first.value;
    direct_update.event = "直接更新";
    Check(repository.Update(direct_update).ok(), "Update 应成功更新规则");

    ScheduleRule missing_update = DailyRule();
    missing_update.id = 999999;
    Check(repository.Update(missing_update).code == ErrorCode::kNotFound, "Update 不存在规则应返回未找到");
    Check(repository.FindById(999999).status.code == ErrorCode::kNotFound, "FindById 不存在规则应返回未找到");

    ScheduleException bad_exception;
    bad_exception.rule_id = 0;
    Check(repository.Upsert(bad_exception).status.code == ErrorCode::kInvalidArgument, "例外非法规则标识应被拒绝");

    const auto missing_exception = repository.FindByRuleAndTime(rule_id, DateTime{std::chrono::seconds{123}});
    Check(missing_exception.ok() && !missing_exception.value->has_value(), "未命中例外应返回空值");

    const auto empty_list = repository.FindByRule(rule_id);
    Check(empty_list.ok() && empty_list.value->empty(), "无例外的规则应返回空列表");

    Schedule bad_instance = FirstInstance(rule_id);
    bad_instance.event = "";
    Check(repository.CreateNextInstance(bad_instance, std::nullopt).status.code == ErrorCode::kInvalidArgument,
          "空实例名应被拒绝");
    Schedule no_rule_instance = FirstInstance(0);
    Check(repository.CreateNextInstance(no_rule_instance, std::nullopt).status.code == ErrorCode::kInvalidArgument,
          "无规则标识实例应被拒绝");

    const auto next = repository.CreateNextInstance(FirstInstance(rule_id), std::nullopt);
    Check(next.ok() && next.value->id > 0, "无关联例外的实例创建应成功");

    int64_t cancelled = 0;
    Check(repository.CancelRuleAndInstances(0, cancelled).code == ErrorCode::kInvalidArgument,
          "取消非法规则标识应被拒绝");
    Check(repository.CancelRuleAndInstances(999999, cancelled).code == ErrorCode::kNotFound,
          "取消不存在规则应返回未找到");
    Check(repository.CancelRuleAndInstances(rule_id, cancelled).ok() && cancelled >= 1, "取消规则应成功");

    ScheduleRule bad_id = DailyRule();
    bad_id.id = 0;
    Check(repository.Update(bad_id).code == ErrorCode::kInvalidArgument, "更新无标识规则应被拒绝");
    Check(repository.UpdateAndRebuild(bad_id, std::nullopt).status.code == ErrorCode::kInvalidArgument,
          "重建无标识规则应被拒绝");
}

/**
 * @brief 验证数据库未打开时各仓储方法的不可用分支。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckClosedDatabaseBranches(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    SqliteScheduleRuleRepository repository(database);
    Check(!database.IsOpen(), "未打开的数据库应处于关闭状态");

    const ScheduleRule rule = DailyRule();
    Check(repository.Initialize().code == ErrorCode::kUnavailable, "关闭数据库时 Initialize 应不可用");
    Check(repository.Insert(rule).status.code == ErrorCode::kUnavailable, "关闭数据库时 Insert 应不可用");
    Check(repository.Update(rule).code == ErrorCode::kUnavailable, "关闭数据库时 Update 应不可用");
    Check(repository.FindAll().status.code == ErrorCode::kUnavailable, "关闭数据库时 FindAll 应不可用");
    Check(repository.FindById(1).status.code == ErrorCode::kUnavailable, "关闭数据库时 FindById 应不可用");
    Check(repository.CreateWithFirstInstance(rule, std::nullopt).status.code == ErrorCode::kUnavailable,
          "关闭数据库时 CreateWithFirstInstance 应不可用");
    Check(repository.UpdateAndRebuild(rule, std::nullopt).status.code == ErrorCode::kUnavailable,
          "关闭数据库时 UpdateAndRebuild 应不可用");
    int64_t cancelled = 0;
    Check(repository.CancelRuleAndInstances(1, cancelled).code == ErrorCode::kUnavailable,
          "关闭数据库时 CancelRuleAndInstances 应不可用");
    Check(repository.CreateNextInstance(FirstInstance(1), std::nullopt).status.code == ErrorCode::kUnavailable,
          "关闭数据库时 CreateNextInstance 应不可用");
    ScheduleException exception;
    exception.rule_id = 1;
    Check(repository.Upsert(exception).status.code == ErrorCode::kUnavailable, "关闭数据库时 Upsert 应不可用");
    Check(repository.FindByRule(1).status.code == ErrorCode::kUnavailable, "关闭数据库时 FindByRule 应不可用");
    Check(repository.FindByRuleAndTime(1, DateTime{}).status.code == ErrorCode::kUnavailable,
          "关闭数据库时 FindByRuleAndTime 应不可用");
    Check(repository.DeleteFuture(1, DateTime{}).code == ErrorCode::kUnavailable, "关闭数据库时 DeleteFuture 应不可用");
}

/**
 * @brief 验证规则仓储事务中各步骤违反 SQLite 约束时回滚并透传错误。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckRuleRepositoryRollbackBranches(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "回滚分支测试应打开数据库");
    SqliteScheduleRuleRepository repository(database);
    Check(repository.Initialize().ok(), "回滚分支测试应初始化表结构");

    // 规则插入违反 CHECK 约束（非法频率）→ 事务回滚。
    ScheduleRule bad_freq = DailyRule();
    bad_freq.freq_type = static_cast<Frequency>(99);
    Check(repository.CreateWithFirstInstance(bad_freq, std::nullopt).status.code == ErrorCode::kAlreadyExists,
          "规则插入违反约束时应回滚并返回约束冲突");

    // 首条实例插入违反 CHECK 约束（非法状态）→ 事务回滚。
    Schedule bad_first = FirstInstance(0);
    bad_first.status = static_cast<ScheduleStatus>(99);
    Check(repository.CreateWithFirstInstance(DailyRule(), bad_first).status.code == ErrorCode::kAlreadyExists,
          "首条实例插入违反约束时应回滚并返回约束冲突");

    const auto created = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
    Check(created.ok() && created.value->id > 0, "回滚分支测试应创建基准规则");
    const ScheduleRuleId rule_id = created.value->id;

    // 更新规则违反 CHECK 约束（事件过长）→ 事务回滚。
    ScheduleRule too_long = *created.value;
    too_long.event = std::string(101, 'x');
    Check(repository.UpdateAndRebuild(too_long, std::nullopt).status.code == ErrorCode::kAlreadyExists,
          "更新规则违反约束时应回滚并返回约束冲突");

    // 更新不存在的规则：影响行数非 1 → 事务回滚并返回未找到。
    ScheduleRule missing = DailyRule();
    missing.id = 999999;
    Check(repository.UpdateAndRebuild(missing, std::nullopt).status.code == ErrorCode::kNotFound,
          "更新不存在规则应回滚并返回未找到");

    // 更新重建时首条实例插入违反约束 → 事务回滚。
    ScheduleRule valid_update = *created.value;
    valid_update.event = "更新实例失败";
    Schedule bad_rebuild_first = FirstInstance(rule_id);
    bad_rebuild_first.status = static_cast<ScheduleStatus>(99);
    Check(repository.UpdateAndRebuild(valid_update, bad_rebuild_first).status.code == ErrorCode::kAlreadyExists,
          "更新重建首条实例违反约束时应回滚并返回约束冲突");

    // 创建下一条实例违反约束（非法状态）→ 事务回滚。
    Schedule bad_next = FirstInstance(rule_id);
    bad_next.status = static_cast<ScheduleStatus>(99);
    Check(repository.CreateNextInstance(bad_next, std::nullopt).status.code == ErrorCode::kAlreadyExists,
          "创建下一条实例违反约束时应回滚并返回约束冲突");

    // 关联例外写入违反约束（非法类型）→ 事务回滚。
    Schedule valid_next = FirstInstance(rule_id);
    ScheduleException bad_linked;
    bad_linked.rule_id = rule_id;
    bad_linked.original_start_time = DateTime{std::chrono::seconds{4'071'258'000}};
    bad_linked.type = static_cast<ExceptionType>(99);
    Check(repository.CreateNextInstance(valid_next, bad_linked).status.code == ErrorCode::kAlreadyExists,
          "关联例外写入违反约束时应回滚并返回约束冲突");
}

/**
 * @brief 验证规则仓储在表结构缺失时透传 SQL 编译错误。
 * @return 无。
 */
void CheckRuleRepositorySqlFailures() {
    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "规则表失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "规则表失败分支应初始化表结构");
        Check(database.Execute("DROP TABLE schedule_rule").ok(), "应删除规则表制造 SQL 错误");
        Check(repository.FindAll().status.code == ErrorCode::kInternal, "FindAll 应透传规则表缺失错误");
        Check(repository.FindById(1).status.code == ErrorCode::kInternal, "FindById 应透传规则表缺失错误");
        ScheduleRule fake = DailyRule();
        fake.id = 1;
        Check(repository.UpdateAndRebuild(fake, std::nullopt).status.code == ErrorCode::kInternal,
              "UpdateAndRebuild 应透传规则表缺失错误");
    }
    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "例外表失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "例外表失败分支应初始化表结构");
        Check(database.Execute("DROP TABLE schedule_rule_exception").ok(), "应删除例外表制造 SQL 错误");
        Check(repository.FindByRule(1).status.code == ErrorCode::kInternal, "FindByRule 应透传例外表缺失错误");
        Check(repository.FindByRuleAndTime(1, DateTime{}).status.code == ErrorCode::kInternal,
              "FindByRuleAndTime 应透传例外表缺失错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "插入规则失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "插入规则失败分支应初始化表结构");
        Check(database.Execute("DROP TABLE schedule_rule").ok(), "应删除规则表制造插入 SQL 错误");
        Check(repository.Insert(DailyRule()).status.code == ErrorCode::kInternal, "Insert 应透传插入规则 SQL 编译错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "更新规则失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "更新规则失败分支应初始化表结构");
        Check(database.Execute("DROP TABLE schedule_rule").ok(), "应删除规则表制造更新 SQL 错误");
        ScheduleRule rule = DailyRule();
        rule.id = 1;
        Check(repository.Update(rule).code == ErrorCode::kInternal, "Update 应透传更新规则 SQL 编译错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "删除未来例外失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "删除未来例外失败分支应初始化表结构");
        Check(database.Execute("DROP TABLE schedule_rule_exception").ok(), "应删除例外表制造删除 SQL 错误");
        Check(repository.DeleteFuture(1, DateTime{}).code == ErrorCode::kInternal,
              "DeleteFuture 应透传删除未来例外 SQL 编译错误");
    }
}

/**
 * @brief 验证更新重建时删除未来实例/例外语句的 SQL 错误回滚分支。
 * @return 无。
 */
void CheckRuleRepositoryDeleteFailures() {
    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "删除日程失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "删除日程失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
        Check(created.ok(), "应创建基准规则");
        ScheduleRule update = *created.value;
        update.event = "删除日程失败";
        Check(database.Execute("DROP TABLE schedule").ok(), "应删除日程表制造 DELETE 错误");
        Check(repository.UpdateAndRebuild(update, std::nullopt).status.code == ErrorCode::kInternal,
              "UpdateAndRebuild 应透传删除未来实例语句错误");
    }
    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "删除例外失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "删除例外失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
        Check(created.ok(), "应创建基准规则");
        ScheduleRule update = *created.value;
        update.event = "删除例外失败";
        Check(database.Execute("DROP TABLE schedule_rule_exception").ok(), "应删除例外表制造 DELETE 错误");
        Check(repository.UpdateAndRebuild(update, std::nullopt).status.code == ErrorCode::kInternal,
              "UpdateAndRebuild 应透传删除未来例外语句错误");
    }
}

/**
 * @brief 验证规则仓储在语句执行阶段失败时透传错误并回滚。
 * @return 无。
 */
void CheckRuleRepositoryStepFailures() {
    const char* create_rule_insert_trigger =
        "CREATE TRIGGER reject_rule_insert BEFORE INSERT ON schedule_rule "
        "BEGIN SELECT RAISE(ABORT, 'rule insert blocked'); END";
    const char* create_rule_update_trigger =
        "CREATE TRIGGER reject_rule_update BEFORE UPDATE ON schedule_rule "
        "BEGIN SELECT RAISE(ABORT, 'rule update blocked'); END";
    const char* create_schedule_insert_trigger =
        "CREATE TRIGGER reject_schedule_insert BEFORE INSERT ON schedule "
        "BEGIN SELECT RAISE(ABORT, 'schedule insert blocked'); END";
    const char* create_exception_insert_trigger =
        "CREATE TRIGGER reject_exception_insert BEFORE INSERT ON schedule_rule_exception "
        "BEGIN SELECT RAISE(ABORT, 'exception insert blocked'); END";
    const char* create_exception_delete_trigger =
        "CREATE TRIGGER reject_exception_delete BEFORE DELETE ON schedule_rule_exception "
        "BEGIN SELECT RAISE(ABORT, 'exception delete blocked'); END";

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "插入规则执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "插入规则执行失败分支应初始化表结构");
        Check(database.Execute(create_rule_insert_trigger).ok(), "应创建规则插入拒绝触发器");
        Check(repository.Insert(DailyRule()).status.code == ErrorCode::kAlreadyExists, "Insert 应透传插入规则执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "更新规则执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "更新规则执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
        Check(created.ok(), "更新规则执行失败分支应创建基准规则");
        ScheduleRule update = *created.value;
        update.event = "更新触发失败";
        Check(database.Execute(create_rule_update_trigger).ok(), "应创建规则更新拒绝触发器");
        Check(repository.Update(update).code == ErrorCode::kAlreadyExists, "Update 应透传更新规则执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "创建首条实例执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "创建首条实例执行失败分支应初始化表结构");
        Check(database.Execute(create_schedule_insert_trigger).ok(), "应创建日程插入拒绝触发器");
        Check(
            repository.CreateWithFirstInstance(DailyRule(), FirstInstance(0)).status.code == ErrorCode::kAlreadyExists,
            "CreateWithFirstInstance 应透传首条实例插入执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "更新重建执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "更新重建执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
        Check(created.ok(), "更新重建执行失败分支应创建基准规则");
        ScheduleRule update = *created.value;
        update.event = "更新重建触发失败";
        Check(database.Execute(create_rule_update_trigger).ok(), "应创建规则更新拒绝触发器");
        Check(repository.UpdateAndRebuild(update, std::nullopt).status.code == ErrorCode::kAlreadyExists,
              "UpdateAndRebuild 应透传更新规则执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "创建下一条实例执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "创建下一条实例执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
        Check(created.ok(), "创建下一条实例执行失败分支应创建基准规则");
        Check(database.Execute(create_schedule_insert_trigger).ok(), "应创建日程插入拒绝触发器");
        Check(repository.CreateNextInstance(FirstInstance(created.value->id), std::nullopt).status.code ==
                  ErrorCode::kAlreadyExists,
              "CreateNextInstance 应透传日程插入执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "取消规则执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "取消规则执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), FirstInstance(0));
        Check(created.ok(), "取消规则执行失败分支应创建基准规则");
        Check(database.Execute(create_rule_update_trigger).ok(), "应创建规则更新拒绝触发器");
        int64_t cancelled = 0;
        Check(repository.CancelRuleAndInstances(created.value->id, cancelled).code == ErrorCode::kAlreadyExists,
              "CancelRuleAndInstances 应透传取消规则执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "例外 Upsert 执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "例外 Upsert 执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
        Check(created.ok(), "例外 Upsert 执行失败分支应创建基准规则");
        Check(database.Execute(create_exception_insert_trigger).ok(), "应创建例外插入拒绝触发器");
        Check(repository.Upsert(ModifyException(created.value->id)).status.code == ErrorCode::kAlreadyExists,
              "Upsert 应透传例外写入执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "删除未来例外执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "删除未来例外执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
        Check(created.ok(), "删除未来例外执行失败分支应创建基准规则");
        Check(repository.Upsert(ModifyException(created.value->id)).ok(), "删除未来例外执行失败分支应写入例外");
        Check(database.Execute(create_exception_delete_trigger).ok(), "应创建例外删除拒绝触发器");
        Check(repository.DeleteFuture(created.value->id, DateTime{}).code == ErrorCode::kAlreadyExists,
              "DeleteFuture 应透传删除未来例外执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "取消清理例外执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "取消清理例外执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), FirstInstance(0));
        Check(created.ok(), "取消清理例外执行失败分支应创建基准规则");
        Check(repository.Upsert(ModifyException(created.value->id)).ok(), "取消清理例外执行失败分支应写入例外");
        Check(database.Execute(create_exception_delete_trigger).ok(), "应创建例外删除拒绝触发器");
        int64_t cancelled = 0;
        Check(repository.CancelRuleAndInstances(created.value->id, cancelled).code == ErrorCode::kAlreadyExists,
              "CancelRuleAndInstances 应透传清理例外执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "取消实例执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "取消实例执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), FirstInstance(0));
        Check(created.ok(), "取消实例执行失败分支应创建基准规则");
        Check(database.Execute(create_rule_update_trigger).ok(), "应创建规则更新拒绝触发器");
        Check(database.Execute("DROP TRIGGER reject_rule_update").ok(), "应删除规则更新拒绝触发器");
        Check(database
                  .Execute("CREATE TRIGGER reject_schedule_cancel BEFORE UPDATE OF status ON schedule "
                           "BEGIN SELECT RAISE(ABORT, 'schedule cancel blocked'); END")
                  .ok(),
              "应创建日程取消拒绝触发器");
        int64_t cancelled = 0;
        Check(repository.CancelRuleAndInstances(created.value->id, cancelled).code == ErrorCode::kAlreadyExists,
              "CancelRuleAndInstances 应透传取消实例执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "删除未来日程执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "删除未来日程执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), FirstInstance(0));
        Check(created.ok(), "删除未来日程执行失败分支应创建基准规则");
        Check(database
                  .Execute("CREATE TRIGGER reject_schedule_delete BEFORE DELETE ON schedule "
                           "BEGIN SELECT RAISE(ABORT, 'schedule delete blocked'); END")
                  .ok(),
              "应创建日程删除拒绝触发器");
        ScheduleRule update = *created.value;
        update.event = "删除未来日程触发失败";
        Check(repository.UpdateAndRebuild(update, std::nullopt).status.code == ErrorCode::kAlreadyExists,
              "UpdateAndRebuild 应透传删除未来日程执行错误");
    }

    {
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "删除未来例外执行失败分支应打开数据库");
        SqliteScheduleRuleRepository repository(database);
        Check(repository.Initialize().ok(), "删除未来例外执行失败分支应初始化表结构");
        const auto created = repository.CreateWithFirstInstance(DailyRule(), std::nullopt);
        Check(created.ok(), "删除未来例外执行失败分支应创建基准规则");
        Check(repository.Upsert(ModifyException(created.value->id)).ok(), "删除未来例外执行失败分支应写入例外");
        Check(database
                  .Execute("CREATE TRIGGER reject_future_exception_delete BEFORE DELETE ON schedule_rule_exception "
                           "BEGIN SELECT RAISE(ABORT, 'future exception delete blocked'); END")
                  .ok(),
              "应创建未来例外删除拒绝触发器");
        ScheduleRule update = *created.value;
        update.event = "删除未来例外触发失败";
        Check(repository.UpdateAndRebuild(update, std::nullopt).status.code == ErrorCode::kAlreadyExists,
              "UpdateAndRebuild 应透传删除未来例外执行错误");
    }
}

/**
 * @brief 验证删除未来例外时不会误删历史例外。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckDeleteFutureKeepsPastException(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "DeleteFuture 测试应打开数据库");
    SqliteScheduleRuleRepository repository(database);
    Check(repository.Initialize().ok(), "DeleteFuture 测试应初始化表结构");

    const auto created = repository.Insert(DailyRule());
    Check(created.ok(), "DeleteFuture 测试应创建规则");
    const ScheduleRuleId rule_id = created.value->id;
    const DateTime cutoff = DateTime{std::chrono::seconds{4'071'258'000}};

    ScheduleException past = ModifyException(rule_id);
    past.original_start_time = DateTime{std::chrono::seconds{4'071'254'400}};
    ScheduleException future = ModifyException(rule_id);
    future.original_start_time = cutoff;
    Check(repository.Upsert(past).ok() && repository.Upsert(future).ok(), "DeleteFuture 测试应写入前后两个例外");

    Check(repository.DeleteFuture(rule_id, cutoff).ok(), "DeleteFuture 应成功删除截止时间之后的例外");
    const auto remaining = repository.FindByRule(rule_id);
    Check(remaining.ok() && remaining.value->size() == 1 &&
              remaining.value->front().original_start_time == past.original_start_time,
          "DeleteFuture 不应删除截止时间之前的历史例外");
}

/**
 * @brief 验证规则和例外列表查询能够完整读取多行结果。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckListQueriesReadMultipleRows(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "多行查询测试应打开数据库");
    SqliteScheduleRuleRepository repository(database);
    Check(repository.Initialize().ok(), "多行查询测试应初始化表结构");

    const auto first = repository.Insert(DailyRule());
    ScheduleRule second_rule = DailyRule();
    second_rule.event = "第二条规则";
    const auto second = repository.Insert(second_rule);
    Check(first.ok() && second.ok(), "多行查询测试应插入两条规则");

    ScheduleException first_exception = ModifyException(first.value->id);
    ScheduleException second_exception = first_exception;
    second_exception.original_start_time = DateTime{std::chrono::seconds{4'071'261'600}};
    Check(repository.Upsert(first_exception).ok() && repository.Upsert(second_exception).ok(),
          "多行查询测试应插入两个例外");

    const auto rules = repository.FindAll();
    Check(rules.ok() && rules.value->size() == 2, "FindAll 应读取全部规则行");
    const auto exceptions = repository.FindByRule(first.value->id);
    Check(exceptions.ok() && exceptions.value->size() == 2, "FindByRule 应读取同一规则的全部例外行");
}

}  // namespace

/**
 * @brief 执行 SQLite 周期规则仓储最小链路测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    const TemporaryDatabaseFile temporary = MakeTemporaryDatabaseFile();
    SqliteDatabase database(temporary.path.string());
    Check(database.Open().ok(), "应成功打开真实 SQLite 数据库文件");
    SqliteScheduleRuleRepository repository(database);
    Check(repository.Initialize().ok(), "应成功创建周期规则表结构");

    const auto created = repository.CreateWithFirstInstance(DailyRule(), FirstInstance(0));
    Check(created.ok() && created.value->id > 0 && created.value->created_at.time_since_epoch().count() != 0,
          "创建周期规则应返回数据库生成的 ID 和时间戳");
    const ScheduleRuleId rule_id = created.value->id;

    const auto loaded = repository.FindById(rule_id);
    const LocalDate expected_start = LocalDate{2099, 1, 1};
    Check(loaded.ok() && loaded.value->event == "每日例会" && loaded.value->location == "会议室" &&
              loaded.value->freq_type == Frequency::kDaily && loaded.value->start_date.year == expected_start.year &&
              loaded.value->start_date.month == expected_start.month &&
              loaded.value->start_date.day == expected_start.day,
          "按标识读取规则应还原完整字段");

    const auto all = repository.FindAll();
    Check(all.ok() && all.value->size() == 1 && all.value->front().id == rule_id, "读取全部规则应返回刚创建的规则");

    ScheduleRule updated = *created.value;
    updated.event = "新每日例会";
    updated.notes = "更新后的备注";
    const auto rebuilt = repository.UpdateAndRebuild(updated, FirstInstance(rule_id));
    Check(rebuilt.ok() && rebuilt.value->event == "新每日例会" && rebuilt.value->notes == "更新后的备注",
          "更新规则应保存修改字段并重建首条实例");

    ScheduleException exception;
    exception.rule_id = rule_id;
    exception.original_start_time = DateTime{std::chrono::seconds{4'071'258'000}};
    exception.type = ExceptionType::kModify;
    exception.override_event = "修改后的第二场";
    const auto upserted = repository.Upsert(exception);
    Check(upserted.ok() && upserted.value->id > 0 && upserted.value->override_event == "修改后的第二场",
          "周期例外应按逻辑键写入并返回完整例外");
    Check(upserted.value->schedule_id.has_value() == false, "未关联日程的例外不应回写 schedule_id");

    const auto found = repository.FindByRuleAndTime(rule_id, exception.original_start_time);
    Check(found.ok() && found.value->has_value() && found.value->value().id == upserted.value->id,
          "按规则和时间应能读取已写入的例外");
    const auto rule_exceptions = repository.FindByRule(rule_id);
    Check(rule_exceptions.ok() && rule_exceptions.value->size() == 1, "按规则读取例外应命中已写入例外");

    Schedule next = FirstInstance(rule_id);
    next.start_time = DateTime{std::chrono::seconds{4'071'258'000}};
    next.end_time = DateTime{std::chrono::seconds{4'071'261'600}};
    ScheduleException linked = *upserted.value;
    linked.schedule_id = std::nullopt;
    const auto created_next = repository.CreateNextInstance(next, linked);
    if (!created_next.ok()) {
        std::cerr << "CreateNextInstance failed: code=" << static_cast<int>(created_next.status.code)
                  << " message=" << created_next.status.message << '\n';
    }
    Check(created_next.ok(), "创建下一条实例应成功");
    Check(created_next.value->id > 0 && created_next.value->rule_id.has_value() &&
              *created_next.value->rule_id == rule_id,
          "创建下一条实例应回写规则标识");
    const auto linked_exception = repository.FindByRuleAndTime(rule_id, exception.original_start_time);
    Check(linked_exception.ok() && linked_exception.value->has_value() &&
              linked_exception.value->value().schedule_id == created_next.value->id,
          "创建实例时应把关联例外回写 schedule_id");

    const auto future = DateTime{std::chrono::seconds{4'071'258'000}};
    Check(repository.DeleteFuture(rule_id, future).ok(), "删除未来例外应成功执行");
    const auto after_delete = repository.FindByRule(rule_id);
    Check(after_delete.ok() && after_delete.value->empty(), "删除未来例外后规则不应再返回该例外");

    int64_t cancelled_count = -1;
    Check(repository.CancelRuleAndInstances(rule_id, cancelled_count).ok() && cancelled_count >= 1,
          "取消规则应同时取消已物化实例");
    const auto cancelled = repository.FindById(rule_id);
    Check(cancelled.ok() && cancelled.value->status == ScheduleStatus::kCancelled, "取消后的规则状态应持久化为已取消");

    Check(repository.Update(DailyRule()).code == voicelife::ErrorCode::kInvalidArgument, "更新无效规则应返回参数错误");

    const TemporaryDatabaseFile mapper_file = MakeTemporaryDatabaseFile();
    CheckRuleMapperValidation(mapper_file.path);
    const TemporaryDatabaseFile full_rule_file = MakeTemporaryDatabaseFile();
    CheckFullRuleRoundTrip(full_rule_file.path);
    const TemporaryDatabaseFile branches_file = MakeTemporaryDatabaseFile();
    CheckRuleRepositoryBranches(branches_file.path);
    const TemporaryDatabaseFile closed_file = MakeTemporaryDatabaseFile();
    CheckClosedDatabaseBranches(closed_file.path);
    const TemporaryDatabaseFile rollback_file = MakeTemporaryDatabaseFile();
    CheckRuleRepositoryRollbackBranches(rollback_file.path);
    CheckRuleRepositorySqlFailures();
    CheckRuleRepositoryDeleteFailures();
    CheckRuleRepositoryStepFailures();
    const TemporaryDatabaseFile delete_future_file = MakeTemporaryDatabaseFile();
    CheckDeleteFutureKeepsPastException(delete_future_file.path);
    const TemporaryDatabaseFile list_queries_file = MakeTemporaryDatabaseFile();
    CheckListQueriesReadMultipleRows(list_queries_file.path);
    return 0;
}
