#include "voicelife/timing/timing_task_service.h"

#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/contracts/status.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::RegisterTimerTaskCommand;
using voicelife::timing::ReminderType;
using voicelife::timing::TimingClockPort;
using voicelife::timing::TimingIdGeneratorPort;
using voicelife::timing::TimingTaskStatus;

namespace {

class FixedTimingClock final : public TimingClockPort {
   public:
    int64_t Now() const override { return 1785740000; }
};

class FixedTimingIdGenerator final : public TimingIdGeneratorPort {
   public:
    std::string NextTaskId() override { return "task-1"; }
    std::string NextReminderRuleId() override { return "rule-" + std::to_string(next_rule_++); }

   private:
    int next_rule_ = 1;
};

}  // namespace

int main() {
    InMemoryTimingTaskStore store;
    FixedTimingClock clock;
    FixedTimingIdGenerator ids;
    DefaultTimingTaskService service(store, clock, ids);

    const auto registered = service.RegisterTimerTask({
        .schedule_id = "schedule-1",
        .start_at = 1785747600,
        .time_zone = "Asia/Shanghai",
    });

    Check(registered.ok(), "合法的一次性日程应注册成功");
    Check(!registered.value->task_id.empty(), "注册结果应返回任务标识");
    Check(registered.value->status == TimingTaskStatus::kActive, "新注册任务应处于 active 状态");
    Check(registered.value->next_trigger_at == 1785747600, "下一次触发时间应等于日程开始时间");

    const auto task = store.FindTask(registered.value->task_id);
    Check(task.ok() && task.value->schedule_id == "schedule-1", "注册任务应被持久化");

    const auto rules = store.ListRules(registered.value->task_id);
    Check(rules.ok() && rules.value->size() == 2, "注册应原子创建两条默认提醒规则");
    bool has_weak_rule = false;
    bool has_strong_rule = false;
    for (const auto& rule : *rules.value) {
        has_weak_rule = has_weak_rule || (rule.type == ReminderType::kWeak && rule.offset_minutes == -10);
        has_strong_rule = has_strong_rule || (rule.type == ReminderType::kStrong && rule.offset_minutes == 0);
    }
    Check(has_weak_rule, "默认弱提醒应提前十分钟");
    Check(has_strong_rule, "默认强提醒应在事件开始时触发");

    const auto invalid = service.RegisterTimerTask({});
    Check(invalid.status.code == ErrorCode::kInvalidArgument, "注册服务应返回领域参数校验错误");

    const auto duplicate = service.RegisterTimerTask({
        .schedule_id = "schedule-2",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    Check(duplicate.status.code == ErrorCode::kConflict, "注册服务应返回存储冲突错误");

    InMemoryTimingTaskStore recurring_store;
    FixedTimingIdGenerator recurring_ids;
    DefaultTimingTaskService recurring_service(recurring_store, clock, recurring_ids);
    const auto recurring = recurring_service.RegisterTimerTask({
        .schedule_id = "schedule-recurring",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
        .recurrence =
            {
                .frequency = RecurrenceFrequency::kDay,
                .start_at = 1785920400,
            },
    });
    Check(recurring.ok(), "带显式周期起点的任务应注册成功");
    const auto stored_recurring = recurring_store.FindTask(recurring.value->task_id);
    Check(stored_recurring.ok() && stored_recurring.value->recurrence.start_at == 1785920400,
          "注册服务应保留显式周期起点");
    return 0;
}
