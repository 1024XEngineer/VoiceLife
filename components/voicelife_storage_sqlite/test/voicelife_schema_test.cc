#include "voicelife/storage_sqlite/voicelife_schema.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "support/test_support.h"
#include "voicelife/storage_sqlite/sqlite_schema.h"

using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteSchema;
using voicelife::storage_sqlite::SqliteStep;
using voicelife::storage_sqlite::VoiceLifeSchema;
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
    Check(ScalarInt64(database, "SELECT COUNT(*) FROM pragma_table_info('schedule')") == 14,
          "日程实例表应包含推迟状态在内的十四个字段");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM pragma_table_info('schedule') "
                      "WHERE name='reminder_task_id' AND type='INTEGER'") == 1,
          "版本五应增加可空 INTEGER reminder_task_id 字段");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM pragma_table_info('schedule') "
                      "WHERE name='snooze_count' AND type='INTEGER' AND \"notnull\"=1 AND dflt_value='0'") == 1,
          "版本六应增加非空且默认零的 INTEGER snooze_count 字段");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM pragma_table_info('schedule') "
                      "WHERE name='repeat_task_id' AND type='INTEGER' AND \"notnull\"=0") == 1,
          "版本六应增加可空 INTEGER repeat_task_id 字段");
    Check(ScalarInt64(database,
                      "SELECT COUNT(*) FROM pragma_table_info('schedule') "
                      "WHERE name='repeat_trigger_at' AND type='INTEGER' AND \"notnull\"=0") == 1,
          "版本六应增加可空 INTEGER repeat_trigger_at 字段");

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

/** @brief 执行 VoiceLife 产品 Schema 测试。 @return 全部断言通过时返回 0。 */
int RunTests() {
    const TemporaryDatabaseFile version_one = MakeTemporaryDatabaseFile();
    CheckVersionOneSchema(version_one.path);
    const TemporaryDatabaseFile version_four = MakeTemporaryDatabaseFile();
    CheckVersionFourSchema(version_four.path);
    const TemporaryDatabaseFile collision = MakeTemporaryDatabaseFile();
    CheckSchemaCollisionRejected(collision.path);
    return 0;
}

}  // namespace

/** @brief 执行 VoiceLife 产品 Schema 测试入口。 @return 全部断言通过时返回 0。 */
int main() { return RunTests(); }
