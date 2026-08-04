#include "voicelife/timing/timing_task_service.h"

#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/contracts/status.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::RegisterTimerTaskCommand;
using voicelife::timing::ReminderRule;
using voicelife::timing::ReminderType;
using voicelife::timing::TimingClockPort;
using voicelife::timing::TimingIdGeneratorPort;
using voicelife::timing::TimingTask;
using voicelife::timing::TimingTaskId;
using voicelife::timing::TimingTaskStatus;
using voicelife::timing::TimingTaskStorePort;

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

class LookupFailureStore final : public TimingTaskStorePort {
   public:
    Status RegisterTaskWithRules(const TimingTask&, const std::vector<ReminderRule>&) override {
        return Status::Error(ErrorCode::kInternal, "unexpected register");
    }

    Result<TimingTask> FindTaskByRequestId(const std::string&) override {
        return Result<TimingTask>::Failure(ErrorCode::kUnavailable, "store unavailable");
    }

    Result<TimingTask> FindTask(const TimingTaskId&) override {
        return Result<TimingTask>::Failure(ErrorCode::kInternal, "unexpected find");
    }

    Result<std::vector<ReminderRule>> ListRules(const TimingTaskId&) override {
        return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal, "unexpected list");
    }
};

class ConcurrentReplayStore final : public TimingTaskStorePort {
   public:
    explicit ConcurrentReplayStore(TimingTask replayed_task) : replayed_task_(std::move(replayed_task)) {}

    Status RegisterTaskWithRules(const TimingTask&, const std::vector<ReminderRule>&) override {
        return Status::Error(ErrorCode::kConflict, "request registered concurrently");
    }

    Result<TimingTask> FindTaskByRequestId(const std::string&) override {
        if (lookup_count_++ == 0) {
            return Result<TimingTask>::Failure(ErrorCode::kNotFound, "request not found");
        }
        return Result<TimingTask>::Success(replayed_task_);
    }

    Result<TimingTask> FindTask(const TimingTaskId&) override {
        return Result<TimingTask>::Failure(ErrorCode::kInternal, "unexpected find");
    }

    Result<std::vector<ReminderRule>> ListRules(const TimingTaskId&) override {
        return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal, "unexpected list");
    }

   private:
    TimingTask replayed_task_;
    int lookup_count_ = 0;
};

}  // namespace

int main() {
    InMemoryTimingTaskStore store;
    FixedTimingClock clock;
    FixedTimingIdGenerator ids;
    DefaultTimingTaskService service(store, clock, ids);

    const auto registered = service.RegisterTimerTask({
        .request_id = "request-1",
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
    Check(task.ok() && task.value->start_at == 1785747600, "任务应保存唯一的周期锚点");
    Check(task.ok() && task.value->next_trigger_at == task.value->start_at, "首次触发时间应等于任务周期锚点");

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

    const auto replayed = service.RegisterTimerTask({
        .request_id = "request-1",
        .schedule_id = "schedule-1",
        .start_at = 1785747600,
        .time_zone = "Asia/Shanghai",
    });
    Check(replayed.ok() && replayed.value->task_id == registered.value->task_id,
          "相同 request_id 重试应返回原注册结果");

    const auto request_conflict = service.RegisterTimerTask({
        .request_id = "request-1",
        .schedule_id = "schedule-1",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    Check(request_conflict.status.code == ErrorCode::kConflict, "相同 request_id 不能复用到不同注册内容");

    const auto duplicate_schedule = service.RegisterTimerTask({
        .request_id = "request-2",
        .schedule_id = "schedule-1",
        .start_at = 1785747600,
        .time_zone = "Asia/Shanghai",
    });
    Check(duplicate_schedule.status.code == ErrorCode::kConflict, "同一日程使用不同 request_id 不应重复注册");

    const auto invalid = service.RegisterTimerTask({});
    Check(invalid.status.code == ErrorCode::kInvalidArgument, "注册服务应返回领域参数校验错误");

    const auto duplicate = service.RegisterTimerTask({
        .request_id = "request-3",
        .schedule_id = "schedule-2",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    Check(duplicate.status.code == ErrorCode::kConflict, "注册服务应返回存储冲突错误");

    InMemoryTimingTaskStore recurring_store;
    FixedTimingIdGenerator recurring_ids;
    DefaultTimingTaskService recurring_service(recurring_store, clock, recurring_ids);
    const auto recurring = recurring_service.RegisterTimerTask({
        .request_id = "request-recurring",
        .schedule_id = "schedule-recurring",
        .start_at = 1785834000,
        .time_zone = "UTC",
        .recurrence =
            {
                .frequency = RecurrenceFrequency::kDay,
            },
    });
    Check(recurring.ok(), "周期任务应注册成功");
    const auto stored_recurring = recurring_store.FindTask(recurring.value->task_id);
    Check(stored_recurring.ok() && stored_recurring.value->start_at == 1785834000,
          "周期任务应使用命令开始时间作为唯一锚点");
    Check(stored_recurring.ok() && stored_recurring.value->time_zone == "UTC", "周期任务应使用命令顶层时区");

    const auto invalid_day = recurring_service.RegisterTimerTask({
        .request_id = "request-invalid-day",
        .schedule_id = "invalid-day",
        .start_at = 1785834000,
        .time_zone = "UTC",
        .recurrence = {.frequency = RecurrenceFrequency::kDay, .by_weekdays = {1}},
    });
    Check(invalid_day.status.code == ErrorCode::kInvalidArgument, "每日规则不应接受星期筛选");

    const auto invalid_week = recurring_service.RegisterTimerTask({
        .request_id = "request-invalid-week",
        .schedule_id = "invalid-week",
        .start_at = 1785834000,
        .time_zone = "UTC",
        .recurrence = {.frequency = RecurrenceFrequency::kWeek, .by_weekdays = {0}},
    });
    Check(invalid_week.status.code == ErrorCode::kInvalidArgument, "每周规则的星期值必须在 1 到 7 之间");

    const auto invalid_month = recurring_service.RegisterTimerTask({
        .request_id = "request-invalid-month",
        .schedule_id = "invalid-month",
        .start_at = 1785834000,
        .time_zone = "UTC",
        .recurrence = {.frequency = RecurrenceFrequency::kMonth, .by_month_days = {32}},
    });
    Check(invalid_month.status.code == ErrorCode::kInvalidArgument, "每月规则的日期值必须在 1 到 31 之间");

    const auto invalid_year = recurring_service.RegisterTimerTask({
        .request_id = "request-invalid-year",
        .schedule_id = "invalid-year",
        .start_at = 1785834000,
        .time_zone = "UTC",
        .recurrence = {.frequency = RecurrenceFrequency::kYear, .by_months = {13}},
    });
    Check(invalid_year.status.code == ErrorCode::kInvalidArgument, "每年规则的月份值必须在 1 到 12 之间");

    LookupFailureStore lookup_failure_store;
    FixedTimingIdGenerator lookup_failure_ids;
    DefaultTimingTaskService lookup_failure_service(lookup_failure_store, clock, lookup_failure_ids);
    const auto lookup_failure = lookup_failure_service.RegisterTimerTask({
        .request_id = "request-unavailable",
        .schedule_id = "schedule-unavailable",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    Check(lookup_failure.status.code == ErrorCode::kUnavailable, "幂等查询失败时应返回 Store 错误");

    const auto invalid_before_lookup = lookup_failure_service.RegisterTimerTask({
        .request_id = "request-invalid-before-lookup",
        .schedule_id = "",
        .start_at = 0,
        .time_zone = "Asia/Shanghai",
    });
    Check(invalid_before_lookup.status.code == ErrorCode::kInvalidArgument,
          "非法任务参数应在 Store 查询前返回参数错误");

    ConcurrentReplayStore concurrent_replay_store({
        .id = "task-concurrent",
        .schedule_id = "schedule-concurrent",
        .request_id = "request-concurrent",
        .start_at = 1785834000,
        .next_trigger_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    FixedTimingIdGenerator concurrent_replay_ids;
    DefaultTimingTaskService concurrent_replay_service(concurrent_replay_store, clock, concurrent_replay_ids);
    const auto concurrent_replay = concurrent_replay_service.RegisterTimerTask({
        .request_id = "request-concurrent",
        .schedule_id = "schedule-concurrent",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    Check(concurrent_replay.ok() && concurrent_replay.value->task_id == "task-concurrent",
          "并发注册同一请求时应回读并返回已保存任务");

    ConcurrentReplayStore concurrent_conflict_store({
        .id = "task-concurrent-conflict",
        .schedule_id = "another-schedule",
        .request_id = "request-concurrent-conflict",
        .start_at = 1785834000,
        .next_trigger_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    FixedTimingIdGenerator concurrent_conflict_ids;
    DefaultTimingTaskService concurrent_conflict_service(concurrent_conflict_store, clock, concurrent_conflict_ids);
    const auto concurrent_conflict = concurrent_conflict_service.RegisterTimerTask({
        .request_id = "request-concurrent-conflict",
        .schedule_id = "schedule-concurrent-conflict",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    Check(concurrent_conflict.status.code == ErrorCode::kConflict, "并发注册复用 request_id 到不同内容时应返回冲突");
    return 0;
}
