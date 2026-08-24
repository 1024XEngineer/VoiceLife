#include "voicelife/storage_sqlite/sqlite_schedule_reminder_task_repository.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#include "support/test_support.h"
#include "voicelife/schedule/schedule_reminder_task_repository.h"
#include "voicelife/storage_sqlite/sqlite_database.h"
#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::ScheduleReminderActionKind;
using voicelife::schedule::ScheduleReminderBusinessStatus;
using voicelife::schedule::ScheduleReminderTask;
using voicelife::schedule::ScheduleReminderTimerStatus;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteScheduleReminderTaskRepository;
using voicelife::storage_sqlite::SqliteScheduleRepository;
using voicelife::test::Check;

namespace {

struct TemporaryDatabaseFile {
    std::filesystem::path path;
    ~TemporaryDatabaseFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-journal", error);
        std::filesystem::remove(path.string() + "-wal", error);
        std::filesystem::remove(path.string() + "-shm", error);
    }
};

TemporaryDatabaseFile MakeDatabase() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return {.path = std::filesystem::temp_directory_path() /
                    ("voicelife-reminder-repository-" + std::to_string(suffix) + ".db")};
}

DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

ScheduleReminderTask MakeTask(int64_t schedule_id, int64_t chain_id, int attempt, std::string timing_id) {
    return {.schedule_id = schedule_id,
            .chain_id = chain_id,
            .attempt = attempt,
            .timing_task_id = std::move(timing_id),
            .trigger_at = At(2'000 + attempt),
            .business_status = ScheduleReminderBusinessStatus::kScheduled,
            .timer_status = ScheduleReminderTimerStatus::kPending,
            .created_at = At(1'000),
            .updated_at = At(1'000)};
}

void CheckRepository(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    SqliteScheduleReminderTaskRepository repository(database);
    Check(repository.FindAll().status.code == ErrorCode::kUnavailable, "数据库未打开时提醒仓储应返回不可用");
    Check(database.Open().ok(), "提醒仓储测试应打开数据库");
    SqliteScheduleRepository schedule_repository(database);
    Check(schedule_repository.Initialize().ok(), "提醒仓储测试应初始化完整产品 Schema");

    const auto first = repository.Insert(MakeTask(1, 10, 1, "sqlite-timing-1"));
    const auto second = repository.Insert(MakeTask(1, 10, 2, "sqlite-timing-2"));
    const auto third = repository.Insert(MakeTask(2, 20, 1, "sqlite-timing-3"));
    Check(first.ok() && second.ok() && third.ok() && first.value->id > 0, "SQLite 提醒仓储应插入并生成记录 ID");
    Check(repository.FindById(first.value->id).ok() && repository.FindById(99999).status.code == ErrorCode::kNotFound,
          "SQLite 提醒仓储应支持按 ID 查询");
    Check(repository.FindByTimingTaskId("sqlite-timing-1").ok() &&
              repository.FindByTimingTaskId("missing").status.code == ErrorCode::kNotFound,
          "SQLite 提醒仓储应支持按 Timing task 标识查询");
    Check(repository.FindBySchedule(1).ok() && repository.FindBySchedule(1).value->size() == 2 &&
              repository.FindAll().value->size() == 3,
          "SQLite 提醒仓储应支持日程和全量查询");

    auto triggered = *first.value;
    triggered.timer_status = ScheduleReminderTimerStatus::kTriggered;
    triggered.business_status = ScheduleReminderBusinessStatus::kWaitingAcknowledgement;
    triggered.triggered_at = At(2'100);
    triggered.updated_at = At(2'100);
    Check(repository.Update(triggered).ok(), "SQLite 提醒仓储应更新触发任务");
    const auto recent = repository.FindTriggered(At(2'100), At(2'100));
    Check(recent.ok() && recent.value->size() == 1 && recent.value->front().id == triggered.id,
          "SQLite 触发查询应包含闭区间边界");
    triggered.business_status = ScheduleReminderBusinessStatus::kSnoozed;
    triggered.action_operation_id = "sqlite-operation-1";
    triggered.action_kind = ScheduleReminderActionKind::kSnooze;
    triggered.action_occurred_at = At(2'101);
    triggered.action_next_trigger_at = At(2'700);
    Check(repository.Update(triggered).ok(), "SQLite 提醒仓储应保存延迟动作结果");

    auto exhausted = *second.value;
    exhausted.timer_status = ScheduleReminderTimerStatus::kTriggered;
    exhausted.business_status = ScheduleReminderBusinessStatus::kExhausted;
    exhausted.triggered_at = At(2'099);
    Check(repository.Update(exhausted).ok() && repository.FindTriggered(At(2'099), At(2'100)).value->size() == 1,
          "SQLite 触发查询应包含耗尽任务并排除已延迟终态");
    exhausted.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    Check(repository.Update(exhausted).ok() && repository.FindTriggered(At(2'099), At(2'100)).value->empty(),
          "SQLite 触发查询应排除已确认任务");

    Check(!repository.Insert(MakeTask(1, 10, 1, "duplicate-chain-attempt")).ok(), "SQLite 应拒绝重复链和尝试次数");
    Check(!repository.Insert(MakeTask(3, 30, 1, "sqlite-timing-2")).ok(), "SQLite 应拒绝重复 Timing task 标识");
    auto invalid = MakeTask(3, 30, 4, "invalid-attempt");
    Check(repository.Insert(invalid).status.code == ErrorCode::kInvalidArgument, "SQLite 应在写入前拒绝非法尝试次数");
    invalid = MakeTask(3, 30, 1, "invalid-business");
    invalid.business_status = static_cast<ScheduleReminderBusinessStatus>(99);
    Check(repository.Insert(invalid).status.code == ErrorCode::kInvalidArgument, "SQLite 应在写入前拒绝非法业务状态");
    auto duplicate_operation = MakeTask(3, 30, 1, "sqlite-operation-duplicate");
    duplicate_operation.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    duplicate_operation.action_operation_id = "sqlite-operation-1";
    duplicate_operation.action_kind = ScheduleReminderActionKind::kAcknowledge;
    duplicate_operation.action_occurred_at = At(2'200);
    Check(!repository.Insert(duplicate_operation).ok(), "SQLite 应拒绝重复动作 operationId");

    const auto check_invalid_action = [&repository](ScheduleReminderTask task, const char* message) {
        Check(repository.Insert(task).status.code == ErrorCode::kInvalidArgument, message);
    };
    auto invalid_action = MakeTask(4, 40, 1, "sqlite-action-kind-without-operation");
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    check_invalid_action(invalid_action, "SQLite 动作类型不能脱离 operationId 单独保存");
    invalid_action = MakeTask(4, 40, 1, "sqlite-empty-action-operation");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    invalid_action.action_operation_id = "";
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    invalid_action.action_occurred_at = At(2'200);
    check_invalid_action(invalid_action, "SQLite 动作 operationId 不能为空");
    invalid_action = MakeTask(4, 40, 1, "sqlite-operation-without-kind");
    invalid_action.action_operation_id = "sqlite-operation-without-kind";
    invalid_action.action_occurred_at = At(2'200);
    check_invalid_action(invalid_action, "SQLite operationId 必须配套动作类型");
    invalid_action = MakeTask(4, 40, 1, "sqlite-operation-without-time");
    invalid_action.action_operation_id = "sqlite-operation-without-time";
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    check_invalid_action(invalid_action, "SQLite operationId 必须配套动作发生时间");
    invalid_action = MakeTask(4, 40, 1, "sqlite-acknowledge-with-next-trigger");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    invalid_action.action_operation_id = "sqlite-acknowledge-with-next-trigger";
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    invalid_action.action_occurred_at = At(2'200);
    invalid_action.action_next_trigger_at = At(2'800);
    check_invalid_action(invalid_action, "SQLite 确认动作不能保存下一次触发时间");
    invalid_action = MakeTask(4, 40, 1, "sqlite-acknowledge-wrong-status");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kSnoozed;
    invalid_action.action_operation_id = "sqlite-acknowledge-wrong-status";
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    invalid_action.action_occurred_at = At(2'200);
    check_invalid_action(invalid_action, "SQLite 确认动作必须对应已确认业务状态");
    invalid_action = MakeTask(4, 40, 1, "sqlite-snooze-without-next-trigger");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kSnoozed;
    invalid_action.action_operation_id = "sqlite-snooze-without-next-trigger";
    invalid_action.action_kind = ScheduleReminderActionKind::kSnooze;
    invalid_action.action_occurred_at = At(2'200);
    check_invalid_action(invalid_action, "SQLite 延迟动作必须保存下一次触发时间");
    invalid_action = MakeTask(4, 40, 1, "sqlite-snooze-wrong-status");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    invalid_action.action_operation_id = "sqlite-snooze-wrong-status";
    invalid_action.action_kind = ScheduleReminderActionKind::kSnooze;
    invalid_action.action_occurred_at = At(2'200);
    invalid_action.action_next_trigger_at = At(2'800);
    check_invalid_action(invalid_action, "SQLite 延迟动作必须对应已延迟业务状态");
    invalid_action = MakeTask(4, 40, 1, "sqlite-unknown-action-kind");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    invalid_action.action_operation_id = "sqlite-unknown-action-kind";
    invalid_action.action_kind = static_cast<ScheduleReminderActionKind>(99);
    invalid_action.action_occurred_at = At(2'200);
    check_invalid_action(invalid_action, "SQLite 仓储必须拒绝未知动作类型");

    database.Close();
    Check(database.Open().ok(), "SQLite 重启测试应重新打开数据库");
    SqliteScheduleReminderTaskRepository restarted(database);
    Check(restarted.FindAll().ok() && restarted.FindAll().value->size() == 3, "SQLite 重启后应保留提醒任务");
    const auto persisted = restarted.FindById(first.value->id);
    Check(persisted.ok() && persisted.value->timer_status == ScheduleReminderTimerStatus::kTriggered &&
              persisted.value->triggered_at == At(2'100) &&
              persisted.value->business_status == ScheduleReminderBusinessStatus::kSnoozed &&
              persisted.value->action_operation_id == "sqlite-operation-1" &&
              persisted.value->action_kind == ScheduleReminderActionKind::kSnooze &&
              persisted.value->action_occurred_at == At(2'101) && persisted.value->action_next_trigger_at == At(2'700),
          "SQLite 重启后应保留触发状态和完整动作结果");
}

}  // namespace

int main() {
    const TemporaryDatabaseFile database = MakeDatabase();
    CheckRepository(database.path);
    return 0;
}
