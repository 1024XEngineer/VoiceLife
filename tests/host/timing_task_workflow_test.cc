#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/timing/timing_task_runner.h"
#include "voicelife/timing/timing_task_service.h"

using voicelife::test::Check;

namespace {
class Clock final : public voicelife::timing::TimingClockPort { public: int64_t Now() const override { return now; } int64_t now = 1000; };
class Ids final : public voicelife::timing::TimingIdGeneratorPort {
   public: std::string Next(const char* prefix) override { return std::string(prefix) + "-" + std::to_string(next_++); }
   private: int next_ = 1;
};
class Events final : public voicelife::timing::TimingEventPort {
   public: voicelife::Status Publish(const voicelife::timing::TimingEvent& e) override { values.push_back(e); return voicelife::Status::Ok(); }
   std::vector<voicelife::timing::TimingEvent> values;
};
}  // namespace

int main() {
    using namespace voicelife::timing;
    voicelife::test::InMemoryTimingTaskStore store;
    Clock clock;
    Ids ids;
    Events events;
    DefaultTimingTaskService service(store, clock, ids, events);

    const auto first = service.RegisterTimerTask({.schedule_id = "schedule-1", .start_at = 2000});
    Check(first.ok(), "任务应注册成功");
    const auto updated = service.UpdateTimerTask({.task_id = first.value->task_id, .schedule_id = "schedule-1",
                                                   .scope = ChangeScope::kAll, .start_at = 2100});
    Check(updated.ok() && updated.value->next_trigger_at == 2100, "整体更新应重算下次触发时间");
    const auto single = service.UpdateTimerTask({.task_id = first.value->task_id, .scope = ChangeScope::kSingle,
                                                  .start_at = 2150, .target_occurrence_at = 2100});
    Check(single.ok() && !single.value->instance_id.empty(), "单次更新应物化 modified instance");
    const auto view = service.ListCalendarView({.range_start = 2000, .range_end = 2200});
    Check(view.ok() && view.value->total == 1 && view.value->occurrences[0].is_exception,
          "日历视图应合并 occurrence 例外");

    ReminderRule extra{.type = ReminderType::kWeak, .offset_minutes = -30};
    const auto upserted = service.UpsertReminderRules(first.value->task_id, {extra});
    Check(upserted.ok() && upserted.value->size() == 3, "应可追加弱提醒规则");
    std::string extra_id;
    for (const auto& rule : *upserted.value) if (rule.offset_minutes == -30) extra_id = rule.id;
    const auto deleted = service.DeleteReminderRule(extra_id);
    Check(deleted.ok() && deleted.value->status == ReminderRuleStatus::kDisabled, "删除规则应软禁用");

    const auto second = service.RegisterTimerTask({.schedule_id = "schedule-2", .start_at = 2000});
    clock.now = 2000;
    TimingTaskRunner runner(store, clock, ids, events);
    Check(runner.PollDue().ok(), "Runner 应物化到期任务");
    auto triggers = store.ListTriggers();
    ReminderTrigger strong;
    for (auto trigger : *triggers.value) if (trigger.task_id == second.value->task_id && trigger.type == ReminderType::kStrong) { strong = trigger; strong.status = ReminderTriggerStatus::kTriggered; store.UpdateTrigger(strong); }
    const auto listed = service.ListReminderTriggers({.task_id = second.value->task_id});
    Check(listed.ok() && listed.value->total == 2, "应按任务查询提醒触发");
    Check(store.UpsertTriggers({
        {.id = "query-early", .task_id = second.value->task_id, .instance_id = "query-instance",
         .actual_trigger_at = 2500, .status = ReminderTriggerStatus::kPending},
        {.id = "query-late", .task_id = second.value->task_id, .instance_id = "query-instance",
         .actual_trigger_at = 2600, .status = ReminderTriggerStatus::kPending},
    }).ok(), "查询测试 trigger 应保存成功");
    const auto sorted = service.ListReminderTriggers({
        .task_id = second.value->task_id, .status = ReminderTriggerStatus::kPending,
        .sort_order = SortOrder::kDescending,
    });
    Check(sorted.ok() && sorted.value->total == 2 &&
              sorted.value->triggers.front().id == "query-late",
          "提醒查询应支持状态过滤和降序排序");
    const auto snoozed = service.SnoozeReminderTrigger({.reminder_trigger_id = strong.id, .delay_minutes = 5});
    Check(snoozed.ok() && snoozed.value->status == ReminderTriggerStatus::kSnoozed && snoozed.value->actual_trigger_at == 2300,
          "强提醒应可推迟");
    const auto dismissed = service.DismissReminderTrigger(strong.id);
    Check(dismissed.ok() && dismissed.value->status == ReminderTriggerStatus::kDismissed, "推迟中的强提醒应可关闭");

    const auto cancelled_single = service.CancelTimerTask({.task_id = first.value->task_id, .scope = ChangeScope::kSingle,
                                                            .instance_id = single.value->instance_id});
    Check(cancelled_single.ok() && cancelled_single.value->affected_instance_count == 1, "应可取消单次 occurrence");
    const auto cancelled_all = service.CancelTimerTask({.task_id = second.value->task_id, .scope = ChangeScope::kAll});
    Check(cancelled_all.ok() && cancelled_all.value->status == TimingTaskStatus::kTerminated, "整体取消应终止任务");
    auto second_rules = store.ListRules(second.value->task_id);
    ReminderRule foreign = second_rules.value->front();
    const auto foreign_update = service.UpsertReminderRules(first.value->task_id, {foreign});
    Check(foreign_update.status.code == voicelife::ErrorCode::kConflict,
          "已有规则标识不得转移到其他任务");
    ReminderRule promoted{.id = extra_id, .type = ReminderType::kStrong, .offset_minutes = 0};
    const auto duplicate_strong = service.UpsertReminderRules(first.value->task_id, {promoted});
    Check(duplicate_strong.status.code == voicelife::ErrorCode::kConflict,
          "更新已有规则时仍应保持开始时强提醒唯一");
    return 0;
}
