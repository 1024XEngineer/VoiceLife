#include "voicelife/storage_sqlite/voicelife_schema.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include "schema/migrations/v001_create_schedule.h"
#include "schema/migrations/v002_create_schedule_operation.h"
#include "schema/migrations/v003_create_schedule_rule.h"
#include "schema/migrations/v004_create_operation_record.h"
#include "schema/migrations/v005_add_schedule_reminder_task_id.h"
#include "support/test_support.h"
#include "voicelife/storage_sqlite/sqlite_schema.h"

using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteMigration;
using voicelife::storage_sqlite::SqliteSchema;
using voicelife::storage_sqlite::SqliteStep;
using voicelife::storage_sqlite::VoiceLifeSchema;
using voicelife::storage_sqlite::schema::migrations::ApplyV001CreateSchedule;
using voicelife::storage_sqlite::schema::migrations::ApplyV002CreateScheduleOperation;
using voicelife::storage_sqlite::schema::migrations::ApplyV003CreateScheduleRule;
using voicelife::storage_sqlite::schema::migrations::ApplyV004CreateOperationRecord;
using voicelife::storage_sqlite::schema::migrations::ApplyV005AddScheduleReminderTaskId;
using voicelife::test::Check;

namespace {

/** @brief 管理产品 Schema 测试使用的临时数据库文件。 */
struct TemporaryDatabaseFile {
    /** @brief 临时数据库主文件路径。 */
    std::filesystem::path path;

    /** @brief 删除数据库及其附属日志文件。 @return 无。 */
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
                    ("voicelife-product-schema-" + std::to_string(suffix) + ".db")};
}

/**
 * @brief 执行只返回一个整数的查询。
 * @param database 已打开的数据库。
 * @param sql 标量查询 SQL。
 * @return 查询返回的整数。
 */
std::int64_t ScalarInt64(const SqliteDatabase& database, const std::string& sql) {
    auto prepared = database.Prepare(sql);
    Check(prepared.ok(), "产品 Schema 标量查询应成功编译");
    auto statement = std::move(*prepared.value);
    const auto row = statement.Step();
    Check(row.ok() && *row.value == SqliteStep::kRow, "产品 Schema 标量查询应返回一行");
    return statement.ColumnInt64(0);
}

/**
 * @brief 验证正式版本一迁移创建表和字段约束。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckVersionOneSchema(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "产品 Schema 测试应打开数据库");
    Check(VoiceLifeSchema::Initialize(database).ok(), "版本一迁移应成功");

    const auto version = SqliteSchema::ReadVersion(database);
    Check(version.ok() && *version.value == VoiceLifeSchema::kCurrentVersion, "数据库应记录当前产品 Schema 版本");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='schedule'") == 1,
          "版本一应创建日程实例表");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM pragma_table_info('schedule')") == 10,
          "当前日程实例表应包含十个字段且不保存提醒执行状态");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM pragma_table_info('schedule_reminder_task')") == 16,
          "当前提醒任务表应包含事件快照和持久化动作结果字段");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM pragma_table_info('schedule') "
                      "WHERE name='reminder_task_id'") == 0,
          "当前日程表不应包含已迁移的 reminder_task_id 字段");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='schedule_reminder_task'") == 1,
          "当前 Schema 应创建独立提醒任务表");
    Check(
        ScalarInt64(
            database,
            "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='schedule_reminder_task_triggered_idx'") ==
            1,
        "当前 Schema 应创建提醒触发查询索引");
    Check(ScalarInt64(
              database,
              "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='schedule_reminder_task_schedule_idx'") ==
              1,
          "当前 Schema 应创建提醒日程查询索引");

    {
        auto foreign_keys = database.Prepare("PRAGMA foreign_key_list(schedule)");
        Check(foreign_keys.ok(), "应能读取日程实例外键元数据");
        const auto foreign_key_row = foreign_keys.value->Step();
        Check(foreign_key_row.ok() && *foreign_key_row.value == SqliteStep::kDone, "rule_id 不应建立数据库外键");
    }

    Check(database
              .Execute("INSERT INTO schedule "
                       "(rule_id, event, start_time, end_time, location, notes, created_at, updated_at) "
                       "VALUES (NULL, '一次性日程', 2000, 2600, '会议室', '实例备注', 1000, 1000)")
              .ok(),
          "应能保存 rule_id 为空的一次性日程");
    Check(database
              .Execute("INSERT INTO schedule "
                       "(rule_id, event, start_time, created_at, updated_at) "
                       "VALUES (42, '周期实例', 3000, 1100, 1100)")
              .ok(),
          "无外键时应能直接保存非空 rule_id");
    Check(ScalarInt64(database, "SELECT status FROM schedule WHERE rule_id = 42") == 1,
          "未指定状态时应使用 active 默认值");
    Check(database
              .Execute("INSERT INTO schedule (event, created_at, updated_at) "
                       "VALUES ('未安排时间', 1200, 1200)")
              .ok(),
          "start_time 为空时仍应保存日程实例");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM schedule") == 3, "三个合法实例都应成功保存");

    Check(!database
               .Execute("INSERT INTO schedule (event, end_time, created_at, updated_at) "
                        "VALUES ('只有结束时间', 4000, 1200, 1200)")
               .ok(),
          "没有 start_time 时不应单独设置 end_time");
    Check(!database
               .Execute("INSERT INTO schedule (event, start_time, end_time, created_at, updated_at) "
                        "VALUES ('非法结束时间', 4000, 4000, 1200, 1200)")
               .ok(),
          "end_time 不晚于 start_time 时应被数据库约束拒绝");
    Check(!database
               .Execute("INSERT INTO schedule (event, start_time, status, created_at, updated_at) "
                        "VALUES ('非法状态', 4000, 99, 1200, 1200)")
               .ok(),
          "约定之外的状态值应被数据库约束拒绝");

    const std::string long_event(101, 'e');
    Check(!database
               .Execute("INSERT INTO schedule (event, start_time, created_at, updated_at) VALUES ('" + long_event +
                        "', 4000, 1200, 1200)")
               .ok(),
          "超过一百字符的标题应被数据库约束拒绝");
    const std::string long_location(101, 'l');
    Check(!database
               .Execute("INSERT INTO schedule "
                        "(event, start_time, location, created_at, updated_at) VALUES ('地点过长', 4000, '" +
                        long_location + "', 1200, 1200)")
               .ok(),
          "超过一百字符的地点应被数据库约束拒绝");
    const std::string long_notes(201, 'n');
    Check(!database
               .Execute("INSERT INTO schedule "
                        "(event, start_time, notes, created_at, updated_at) VALUES ('备注过长', 4000, '" +
                        long_notes + "', 1200, 1200)")
               .ok(),
          "超过二百字符的备注应被数据库约束拒绝");

    Check(VoiceLifeSchema::Initialize(database).ok(), "重复初始化当前版本应保持幂等");
    database.Close();
    Check(database.Open().ok() && VoiceLifeSchema::Initialize(database).ok(), "重新打开数据库后初始化仍应成功");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM schedule") == 3, "重新打开后应保留已保存的实例");
}

/**
 * @brief 验证版本四迁移重建纯审计结构的操作记录表。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckVersionFourSchema(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "产品 Schema 版本四测试应打开数据库");
    Check(VoiceLifeSchema::Initialize(database).ok(), "版本四迁移应成功");

    const auto version = SqliteSchema::ReadVersion(database);
    Check(version.ok() && *version.value == VoiceLifeSchema::kCurrentVersion, "数据库应记录当前产品 Schema 版本");
    Check(
        ScalarInt64(database, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='operation_record'") == 1,
        "版本四应重建操作记录表");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM pragma_table_info('operation_record')") == 7,
          "操作记录表应包含约定的七个字段");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='operation_record_recent_idx'") ==
              1,
          "版本四应创建操作记录倒序索引");

    // 合法写入：创建操作无 before，修改操作有 before。
    Check(database
              .Execute("INSERT INTO operation_record (entity_type, type, entity_id, label, operated_at, before) "
                       "VALUES (1, 1, 100, '创建日程', 2000000000, NULL)")
              .ok(),
          "创建操作应能写入");
    Check(database
              .Execute("INSERT INTO operation_record (entity_type, type, entity_id, label, operated_at, before) "
                       "VALUES (2, 2, 200, '修改规则', 2000000001, '{\"id\":200}')")
              .ok(),
          "修改操作应能写入 before 快照");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM operation_record") == 2, "两条合法操作都应成功保存");

    // 约束拒绝：非法实体类型 / 非法操作类型 / 非正数 ID / 空名称 / 超长名称 / 创建带 before / 修改缺 before。
    Check(!database
               .Execute("INSERT INTO operation_record (entity_type, type, entity_id, label, operated_at, before) "
                        "VALUES (99, 1, 100, '非法实体类型', 2000000000, NULL)")
               .ok(),
          "约定之外的实体类型应被数据库约束拒绝");
    Check(!database
               .Execute("INSERT INTO operation_record (entity_type, type, entity_id, label, operated_at, before) "
                        "VALUES (1, 99, 100, '非法操作类型', 2000000000, NULL)")
               .ok(),
          "约定之外的操作类型应被数据库约束拒绝");
    Check(!database
               .Execute("INSERT INTO operation_record (entity_type, type, entity_id, label, operated_at, before) "
                        "VALUES (1, 1, 0, '非法标识', 2000000000, NULL)")
               .ok(),
          "非正数实体 ID 应被数据库约束拒绝");
    Check(!database
               .Execute("INSERT INTO operation_record (entity_type, type, entity_id, label, operated_at, before) "
                        "VALUES (1, 1, 100, '', 2000000000, NULL)")
               .ok(),
          "空名称应被数据库约束拒绝");
    const std::string long_label(101, 'a');
    Check(!database
               .Execute("INSERT INTO operation_record (entity_type, type, entity_id, label, operated_at, before) "
                        "VALUES (1, 1, 100, '" +
                        long_label + "', 2000000000, NULL)")
               .ok(),
          "超过一百字符的名称应被数据库约束拒绝");
    Check(!database
               .Execute("INSERT INTO operation_record (entity_type, type, entity_id, label, operated_at, before) "
                        "VALUES (1, 1, 100, '创建带快照', 2000000000, '{\"id\":100}')")
               .ok(),
          "创建操作携带 before 应被数据库约束拒绝");
    Check(!database
               .Execute("INSERT INTO operation_record (entity_type, type, entity_id, label, operated_at, before) "
                        "VALUES (1, 2, 100, '修改缺快照', 2000000000, NULL)")
               .ok(),
          "修改操作缺少 before 应被数据库约束拒绝");

    Check(VoiceLifeSchema::Initialize(database).ok(), "重复初始化当前版本应保持幂等");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM operation_record") == 2, "重新初始化后应保留已保存的操作记录");
}

/**
 * @brief 验证旧的无版本同名表不会被静默接受为正式版本一结构。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckSchemaCollisionRejected(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "结构冲突测试应打开数据库");
    Check(database.Execute("CREATE TABLE schedule (id INTEGER PRIMARY KEY)").ok(), "应能构造无版本旧结构");
    Check(!VoiceLifeSchema::Initialize(database).ok(), "同名旧结构应明确阻止版本一迁移");
    const auto version = SqliteSchema::ReadVersion(database);
    Check(version.ok() && *version.value == 0, "结构冲突后版本号应保持为零");
}

/**
 * @brief 验证真实 v005 数据升级到当前 v008 时提醒任务和日程表均正确迁移。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckVersionFiveToEightMigration(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "v005 到 v008 测试应打开数据库");

    const SqliteMigration migrations[] = {
        {.version = 1, .apply = &ApplyV001CreateSchedule},
        {.version = 2, .apply = &ApplyV002CreateScheduleOperation},
        {.version = 3, .apply = &ApplyV003CreateScheduleRule},
        {.version = 4, .apply = &ApplyV004CreateOperationRecord},
        {.version = 5, .apply = &ApplyV005AddScheduleReminderTaskId},
    };
    Check(SqliteSchema::ApplyMigrations(database, 5, migrations, std::size(migrations)).ok(), "应成功创建 v005 数据库");
    Check(database
              .Execute("INSERT INTO schedule (event, start_time, status, reminder_task_id, created_at, updated_at) "
                       "VALUES ('待迁移提醒', 2000, 1, 77, 1000, 1100)")
              .ok(),
          "v005 应能保存旧提醒任务标识");
    Check(database
              .Execute("INSERT INTO schedule (event, start_time, status, created_at, updated_at) "
                       "VALUES ('无提醒日程', 3000, 1, 1200, 1300)")
              .ok(),
          "v005 应能保存无提醒日程");

    Check(VoiceLifeSchema::Initialize(database).ok(), "v005 到 v008 升级应成功");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM schedule") == 2, "升级后应保留全部日程");
    Check(
        ScalarInt64(database, "SELECT COUNT(*) FROM pragma_table_info('schedule') WHERE name='reminder_task_id'") == 0,
        "升级后日程表应移除旧提醒任务标识");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM schedule_reminder_task") == 1,
          "旧提醒任务应迁移为一条独立提醒记录");
    Check(ScalarInt64(database, "SELECT schedule_id FROM schedule_reminder_task WHERE chain_id=77 AND attempt=1") == 1,
          "迁移提醒应关联原日程");
    Check(ScalarInt64(database, "SELECT trigger_at FROM schedule_reminder_task WHERE chain_id=77") == 2000,
          "迁移提醒应使用原日程开始时间作为触发时间");
    Check(ScalarInt64(database, "SELECT timer_status FROM schedule_reminder_task WHERE chain_id=77") == 1,
          "迁移提醒初始计时状态应为 pending");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM schedule_reminder_task WHERE chain_id=77 AND event='待迁移提醒'") == 1,
          "迁移提醒应保留日程事件快照");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM pragma_table_info('schedule_reminder_task') "
                      "WHERE name IN ('action_operation_id', 'action_kind', 'action_occurred_at', "
                      "'action_next_trigger_at')") == 4,
          "升级后提醒任务表应包含四个动作结果字段");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM schedule_reminder_task WHERE action_operation_id IS NULL") == 1,
          "历史提醒迁移后动作结果应保持为空");
    Check(VoiceLifeSchema::Initialize(database).ok(), "v008 重复初始化应保持幂等");
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM schedule_reminder_task") == 1, "v008 重复初始化不应重复迁移提醒");
}

/**
 * @brief 验证 v005 到当前版本迁移失败时事务回滚且版本号不前进。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckVersionFiveToCurrentRollback(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "迁移回滚测试应打开数据库");
    const SqliteMigration migrations[] = {
        {.version = 1, .apply = &ApplyV001CreateSchedule},
        {.version = 2, .apply = &ApplyV002CreateScheduleOperation},
        {.version = 3, .apply = &ApplyV003CreateScheduleRule},
        {.version = 4, .apply = &ApplyV004CreateOperationRecord},
        {.version = 5, .apply = &ApplyV005AddScheduleReminderTaskId},
    };
    Check(SqliteSchema::ApplyMigrations(database, 5, migrations, std::size(migrations)).ok(),
          "回滚测试应创建 v005 数据库");
    Check(database
              .Execute("INSERT INTO schedule (event, reminder_task_id, created_at, updated_at) "
                       "VALUES ('冲突提醒一', 1, 1000, 1000)")
              .ok(),
          "回滚测试应写入第一条旧提醒数据");
    Check(database
              .Execute("INSERT INTO schedule (event, reminder_task_id, created_at, updated_at) "
                       "VALUES ('冲突提醒二', 1, 1001, 1001)")
              .ok(),
          "回滚测试应写入重复旧提醒数据");
    Check(!VoiceLifeSchema::Initialize(database).ok(), "冲突链标识应使 v006 迁移失败");
    const auto version = SqliteSchema::ReadVersion(database);
    Check(version.ok() && *version.value == 5, "迁移失败后版本号应回滚到 v005");
    Check(
        ScalarInt64(database, "SELECT COUNT(*) FROM pragma_table_info('schedule') WHERE name='reminder_task_id'") == 1,
        "迁移失败后旧日程表结构应保持不变");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='schedule_reminder_task'") == 0,
          "迁移失败后提醒任务表不应残留");
}

/** @brief 执行 VoiceLife 产品 Schema 测试。 @return 全部断言通过时返回 0。 */
int RunTests() {
    const TemporaryDatabaseFile version_one = MakeTemporaryDatabaseFile();
    CheckVersionOneSchema(version_one.path);
    const TemporaryDatabaseFile version_four = MakeTemporaryDatabaseFile();
    CheckVersionFourSchema(version_four.path);
    const TemporaryDatabaseFile migration = MakeTemporaryDatabaseFile();
    CheckVersionFiveToEightMigration(migration.path);
    const TemporaryDatabaseFile rollback = MakeTemporaryDatabaseFile();
    CheckVersionFiveToCurrentRollback(rollback.path);
    const TemporaryDatabaseFile collision = MakeTemporaryDatabaseFile();
    CheckSchemaCollisionRejected(collision.path);
    return 0;
}

}  // namespace

/** @brief 执行 VoiceLife 产品 Schema 测试入口。 @return 全部断言通过时返回 0。 */
int main() { return RunTests(); }
