#include "voicelife/storage_sqlite/sqlite_schedule_rule_repository.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "support/test_support.h"
#include "voicelife/schedule/schedule_types.h"
#include "voicelife/storage_sqlite/sqlite_database.h"

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
    return 0;
}
