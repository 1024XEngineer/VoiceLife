#include "support/test_support.h"
#include "support/timing_store_contract.h"
#include "voicelife/timing_sqlite/sqlite_timing_task_store.h"
#include "voicelife/timing_sqlite/sqlite_timing_task_store_driver.h"

#include <cstdio>

using voicelife::test::Check;

int main() {
    using namespace voicelife::timing;
    using voicelife::timing_sqlite::SqliteTimingTaskStore;
    using voicelife::timing_sqlite::SqliteTimingTaskStoreDriver;
    const std::string path = "/tmp/voicelife-timing-store-test.db";
    std::remove(path.c_str());
    {
        SqliteTimingTaskStore unopened(path);
        Check(unopened.ListTasks().status.code == voicelife::ErrorCode::kUnavailable,
              "Open 前读取应返回 unavailable");
        Check(unopened.UpdateTaskWithEvent({}, {}).code == voicelife::ErrorCode::kUnavailable,
              "Open 前复合写入应返回 unavailable");
    }
    {
        SqliteTimingTaskStore store(path);
        Check(store.Open().ok(), "SQLite store 应初始化 schema");
        voicelife::test::RunTimingStoreContract(store, "sqlite-contract");
        const TimingTask task{
            .id = "task-1", .schedule_id = "schedule-1", .next_trigger_at = 2000,
            .time_zone = "Asia/Shanghai",
            .recurrence = {.frequency = RecurrenceFrequency::kMonth, .start_at = 2000,
                           .time_zone = "Asia/Shanghai", .by_month_days = {1, 15},
                           .by_months = {1, 6, 12}},
            .pending_recurrence = RecurrenceRule{
                .frequency = RecurrenceFrequency::kWeek, .start_at = 3000,
                .time_zone = "Asia/Shanghai", .by_weekdays = {2, 4}},
            .pending_effective_from = 3000,
            .created_at = 1000, .updated_at = 1000,
        };
        const std::vector<ReminderRule> rules{
            {.id = "rule-1", .task_id = task.id, .type = ReminderType::kWeak, .offset_minutes = -10,
             .created_at = 1000, .updated_at = 1000},
            {.id = "rule-2", .task_id = task.id, .type = ReminderType::kStrong, .offset_minutes = 0,
             .max_snooze_count = 3, .created_at = 1000, .updated_at = 1000},
        };
        Check(store.RegisterTaskWithRules(task, rules).ok(), "任务和默认规则应在事务中保存");
        Check(store.RegisterTaskWithRules(task, rules).code == voicelife::ErrorCode::kConflict,
              "重复注册应返回 conflict");
        const TimerInstance instance{
            .id = "instance-1", .task_id = task.id, .planned_at = 2000,
            .planned_end_at = 2300, .actual_trigger_at = 2060,
            .status = TimerInstanceStatus::kModified,
            .override_fields = {.start_at = 2060, .end_at = 2360},
            .last_action_at = 2010, .created_at = 2000, .updated_at = 2010,
        };
        const std::vector<ReminderTrigger> triggers{
            {.id = "trigger-1", .reminder_rule_id = "rule-2", .task_id = task.id,
             .instance_id = instance.id, .type = ReminderType::kStrong,
             .planned_trigger_at = 2000, .actual_trigger_at = 2000,
             .status = ReminderTriggerStatus::kSnoozed, .snooze_count = 1,
             .max_snooze_count = 3, .delivered_at = 2001, .last_action_at = 2002,
             .payload = "{\"kind\":\"voice\"}", .created_at = 2000, .updated_at = 2002},
        };
        auto advanced = task;
        advanced.next_trigger_at = 0;
        advanced.updated_at = 2000;
        const TimingEvent event{
            .event_type = TimingEventType::kInstanceCreated, .event_id = "event-1",
            .task_id = task.id, .instance_id = instance.id, .schedule_id = task.schedule_id,
            .planned_at = instance.planned_at, .trigger_at = 2000,
            .status = TimingEventStatus::kPending, .occurred_at = 2000,
        };
        Check(store.MaterializeOccurrence(instance, triggers, advanced, event).ok(),
              "occurrence、trigger 和任务推进应原子保存");
    }
    {
        SqliteTimingTaskStore reopened(path);
        Check(reopened.Open().ok(), "SQLite store 应可重新打开");
        const auto task = reopened.FindTask("task-1");
        Check(task.ok() && task.value->time_zone == "Asia/Shanghai" &&
                  task.value->recurrence.by_month_days == std::vector<int>({1, 15}) &&
                  task.value->recurrence.by_months == std::vector<int>({1, 6, 12}) &&
                  task.value->pending_recurrence.has_value() &&
                  task.value->pending_recurrence->by_weekdays == std::vector<int>({2, 4}) &&
                  task.value->pending_effective_from == 3000,
              "重启后应恢复当前和待生效 recurrence selectors");
        const auto rules = reopened.ListRules("task-1");
        Check(rules.ok() && rules.value->size() == 2, "重启后应恢复提醒规则");
        Check(reopened.ListInstances("task-1").value->size() == 1 &&
                  reopened.FindTrigger("trigger-1").value->has_value(),
              "重启后应恢复 occurrence 与提醒触发");
        const auto instance = reopened.FindInstance("instance-1");
        Check(instance.ok() && instance.value->has_value() &&
                  instance.value->value().override_fields.start_at == 2060 &&
                  instance.value->value().override_fields.end_at == 2360,
              "重启后应恢复 instance overrides");
        const auto trigger = reopened.FindTrigger("trigger-1");
        Check(trigger.ok() && trigger.value->has_value() &&
                  trigger.value->value().status == ReminderTriggerStatus::kSnoozed &&
                  trigger.value->value().snooze_count == 1 &&
                  trigger.value->value().payload == "{\"kind\":\"voice\"}",
              "重启后应恢复 trigger 运行态字段");
        auto events = reopened.ListPendingEvents();
        Check(events.ok() && events.value->size() == 1 && events.value->front().event_id == "event-1",
              "未发布 outbox 事件应在重启后恢复");
        Check(reopened.MarkEventPublished("event-1").ok(), "outbox 事件应可标记发布");
        Check(reopened.ListPendingEvents().value->empty(), "已发布事件不应再次返回");
    }
    Check(SqliteTimingTaskStoreDriver::ValidateConfig("").code == voicelife::ErrorCode::kInvalidArgument,
          "SQLite driver 应拒绝空路径");
    const auto* descriptor = voicelife::timing_sqlite::FindTimingStoreDriver("sqlite");
    Check(descriptor != nullptr && descriptor->driver == "sqlite" &&
              descriptor->capabilities[0] == "atomic-timing-write" &&
              descriptor->resource_budget.max_open_connections == 1,
          "SQLite driver registry 应暴露稳定名称、能力与资源预算");
    auto memory_store = SqliteTimingTaskStoreDriver::Create(":memory:");
    Check(memory_store.ok() && (*memory_store.value)->ListTasks().ok(),
          "SQLite driver factory 应校验配置并返回已打开的 store");
    std::remove(path.c_str());
    return 0;
}
