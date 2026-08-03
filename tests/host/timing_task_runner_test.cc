#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/timing/timing_task_runner.h"

using voicelife::test::Check;

namespace {
class FixedClock final : public voicelife::timing::TimingClockPort { public: int64_t Now() const override { return now; } int64_t now = 1400; };
class Ids final : public voicelife::timing::TimingIdGeneratorPort {
   public: std::string Next(const char* prefix) override { return std::string(prefix) + "-" + std::to_string(next_++); }
   private: int next_ = 1;
};
class Events final : public voicelife::timing::TimingEventPort {
   public: voicelife::Status Publish(const voicelife::timing::TimingEvent& event) override {
       if (fail_next) {
           fail_next = false;
           return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "downstream unavailable");
       }
       values.push_back(event);
       return voicelife::Status::Ok();
   }
   bool fail_next = false;
   std::vector<voicelife::timing::TimingEvent> values;
};
}  // namespace

int main() {
    using namespace voicelife::timing;
    voicelife::test::InMemoryTimingTaskStore store;
    FixedClock clock;
    Ids ids;
    Events events;
    const TimingTask task{.id = "task-1", .schedule_id = "schedule-1", .next_trigger_at = 2000,
                          .time_zone = "Asia/Shanghai",
                          .recurrence = {.start_at = 2000, .time_zone = "Asia/Shanghai"},
                          .created_at = 1000, .updated_at = 1000};
    const std::vector<ReminderRule> rules{
        {.id = "weak", .task_id = task.id, .type = ReminderType::kWeak, .offset_minutes = -10},
        {.id = "strong", .task_id = task.id, .type = ReminderType::kStrong, .offset_minutes = 0, .max_snooze_count = 3},
    };
    Check(store.RegisterTaskWithRules(task, rules).ok(), "测试任务应保存成功");

    TimingTaskRunner runner(store, clock, ids, events);
    Check(runner.PollDue().ok(), "到期轮询应成功");
    const auto instances = store.ListInstances(task.id);
    const auto triggers = store.ListTriggers();
    Check(instances.ok() && instances.value->size() == 1, "到期任务应生成一个 occurrence 实例");
    Check(triggers.ok() && triggers.value->size() == 2, "实例应按规则生成两个提醒触发");
    int delivered = 0;
    int pending = 0;
    for (const auto& trigger : *triggers.value) {
        if (trigger.status == ReminderTriggerStatus::kDelivered) ++delivered;
        if (trigger.status == ReminderTriggerStatus::kPending) ++pending;
    }
    Check(delivered == 1 && pending == 1, "提前弱提醒应按时送达，强提醒应等待 occurrence 开始");
    clock.now = 2000;
    Check(runner.PollDue().ok(), "重复轮询应成功");
    Check(store.ListInstances(task.id).value->size() == 1 && store.ListTriggers().value->size() == 2,
          "重复轮询不应重复物化 occurrence");
    int triggered_strong = 0;
    const auto triggered = store.ListTriggers();
    for (const auto& trigger : *triggered.value) {
        if (trigger.type == ReminderType::kStrong &&
            trigger.status == ReminderTriggerStatus::kTriggered) {
            ++triggered_strong;
        }
    }
    Check(triggered_strong == 1, "开始时间到达后强提醒应触发");

    const TimingTask modified_task{
        .id = "task-modified", .schedule_id = "schedule-modified", .next_trigger_at = 3600,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.start_at = 3600, .time_zone = "Asia/Shanghai"},
        .created_at = 1000, .updated_at = 1000,
    };
    Check(store.RegisterTaskWithRules(modified_task, {{
        .id = "modified-weak", .task_id = modified_task.id,
        .type = ReminderType::kWeak, .offset_minutes = -10,
    }}).ok(), "修改实例测试任务应保存成功");
    Check(store.UpsertInstance({
        .id = "instance-modified", .task_id = modified_task.id, .planned_at = 3600,
        .actual_trigger_at = 3660, .status = TimerInstanceStatus::kModified,
        .override_fields = {.start_at = 3660}, .created_at = 1000, .updated_at = 1000,
    }).ok(), "预先修改的 occurrence 应保存成功");
    clock.now = 3060;
    Check(runner.PollDue().ok(), "已修改 occurrence 到达提醒窗口时应成功物化");
    int modified_triggers = 0;
    const auto after_modified = store.ListTriggers();
    for (const auto& trigger : *after_modified.value) {
        if (trigger.instance_id == "instance-modified") {
            ++modified_triggers;
            Check(trigger.actual_trigger_at == 3060, "提醒时间应基于 occurrence override 计算");
        }
    }
    Check(modified_triggers == 1, "已修改 occurrence 仍应生成提醒 trigger");

    Check(store.EnqueueEvent({
        .event_type = TimingEventType::kTaskUpdated, .event_id = "retry-event",
        .task_id = task.id, .schedule_id = task.schedule_id,
    }).ok(), "重试测试事件应进入 outbox");
    events.fail_next = true;
    Check(runner.PollDue().code == voicelife::ErrorCode::kUnavailable,
          "下游发布失败应向轮询调用方报告");
    Check(!store.ListPendingEvents().value->empty(), "发布失败的事件应保留在 outbox");
    Check(runner.PollDue().ok(), "下一轮应重试发布未完成事件");
    bool retry_published = false;
    for (const auto& event : events.values) retry_published |= event.event_id == "retry-event";
    Check(retry_published, "outbox 事件应在下游恢复后发布");
    return 0;
}
