#include "voicelife/timing/timing_task_service.h"

#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/contracts/status.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::CancelTimerTaskCommand;
using voicelife::timing::ChangeScope;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::RegisterTimerTaskCommand;
using voicelife::timing::ReminderRule;
using voicelife::timing::ReminderType;
using voicelife::timing::TimerInstance;
using voicelife::timing::TimerInstanceStatus;
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

    Status UpdateTaskWithInstances(const voicelife::timing::TimingTaskUpdateWrite&) override {
        return Status::Error(ErrorCode::kInternal, "unexpected update");
    }

    Result<TimingTask> FindTask(const TimingTaskId&) override {
        return Result<TimingTask>::Failure(ErrorCode::kInternal, "unexpected find");
    }

    Result<std::vector<ReminderRule>> ListRules(const TimingTaskId&) override {
        return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal, "unexpected list");
    }

    Result<std::vector<TimerInstance>> ListInstances(const TimingTaskId&) override {
        return Result<std::vector<TimerInstance>>::Failure(ErrorCode::kInternal, "unexpected list");
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

    Status UpdateTaskWithInstances(const voicelife::timing::TimingTaskUpdateWrite&) override {
        return Status::Error(ErrorCode::kInternal, "unexpected update");
    }

    Result<TimingTask> FindTask(const TimingTaskId&) override {
        return Result<TimingTask>::Failure(ErrorCode::kInternal, "unexpected find");
    }

    Result<std::vector<ReminderRule>> ListRules(const TimingTaskId&) override {
        return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal, "unexpected list");
    }

    Result<std::vector<TimerInstance>> ListInstances(const TimingTaskId&) override {
        return Result<std::vector<TimerInstance>>::Failure(ErrorCode::kInternal, "unexpected list");
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

    InMemoryTimingTaskStore cancel_store;
    FixedTimingIdGenerator cancel_ids;
    DefaultTimingTaskService cancel_service(cancel_store, clock, cancel_ids);
    const auto cancel_registered = cancel_service.RegisterTimerTask({
        .request_id = "request-cancel-single",
        .schedule_id = "schedule-cancel-single",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(cancel_registered.ok(), "single 取消用例应先注册周期任务");
    const auto cancel_task = cancel_store.FindTask(cancel_registered.value->task_id);
    Check(cancel_task.ok(), "single 取消用例应能读回任务");
    Check(cancel_store
              .UpdateTaskWithInstances({
                  .task = *cancel_task.value,
                  .upsert_instances = {TimerInstance{
                      .id = "instance-cancel-single",
                      .task_id = cancel_registered.value->task_id,
                      .planned_at = 1785920400,
                      .status = TimerInstanceStatus::kPending,
                  }},
              })
              .ok(),
          "single 取消用例应能预置待取消实例");

    const auto single_cancel = cancel_service.CancelTimerTask({
        .task_id = cancel_registered.value->task_id,
        .schedule_id = "schedule-cancel-single",
        .change_scope = ChangeScope::kSingle,
        .instance_id = "instance-cancel-single",
        .target_occurrence_at = 1785920400,
    });
    Check(single_cancel.ok(), "single 取消应成功");
    Check(single_cancel.value->task_id == cancel_registered.value->task_id, "single 取消应返回任务标识");
    Check(single_cancel.value->instance_id == "instance-cancel-single", "single 取消应返回被取消实例");
    Check(single_cancel.value->status == TimingTaskStatus::kActive, "single 取消不应终止任务");
    Check(single_cancel.value->affected_instance_count == 1, "single 取消只应影响一个实例");
    const auto task_after_single_cancel = cancel_store.FindTask(cancel_registered.value->task_id);
    Check(task_after_single_cancel.ok() && task_after_single_cancel.value->status == TimingTaskStatus::kActive,
          "single 取消后任务应保持 active");
    const auto instances_after_single_cancel = cancel_store.ListInstances(cancel_registered.value->task_id);
    Check(instances_after_single_cancel.ok() && instances_after_single_cancel.value->size() == 1,
          "single 取消不应创建额外实例");
    Check(instances_after_single_cancel.ok() &&
              instances_after_single_cancel.value->front().status == TimerInstanceStatus::kSkipped,
          "single 取消应将目标实例标记为 skipped");

    InMemoryTimingTaskStore future_cancel_store;
    FixedTimingIdGenerator future_cancel_ids;
    DefaultTimingTaskService future_cancel_service(future_cancel_store, clock, future_cancel_ids);
    const auto future_cancel_registered = future_cancel_service.RegisterTimerTask({
        .request_id = "request-cancel-future",
        .schedule_id = "schedule-cancel-future",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(future_cancel_registered.ok(), "future 取消用例应先注册周期任务");
    const auto future_cancel_task = future_cancel_store.FindTask(future_cancel_registered.value->task_id);
    Check(future_cancel_task.ok(), "future 取消用例应能读回任务");
    Check(future_cancel_store
              .UpdateTaskWithInstances({
                  .task = *future_cancel_task.value,
                  .upsert_instances =
                      {
                          TimerInstance{
                              .id = "instance-before-future-cancel",
                              .task_id = future_cancel_registered.value->task_id,
                              .planned_at = 1785834000,
                              .status = TimerInstanceStatus::kPending,
                          },
                          TimerInstance{
                              .id = "instance-at-future-cancel",
                              .task_id = future_cancel_registered.value->task_id,
                              .planned_at = 1785920400,
                              .status = TimerInstanceStatus::kPending,
                          },
                          TimerInstance{
                              .id = "instance-after-future-cancel",
                              .task_id = future_cancel_registered.value->task_id,
                              .planned_at = 1786006800,
                              .status = TimerInstanceStatus::kPending,
                          },
                      },
              })
              .ok(),
          "future 取消用例应能预置边界前后的实例");

    const auto future_cancel = future_cancel_service.CancelTimerTask({
        .task_id = future_cancel_registered.value->task_id,
        .schedule_id = "schedule-cancel-future",
        .change_scope = ChangeScope::kFuture,
        .effective_from = 1785920400,
    });
    Check(future_cancel.ok(), "future 取消应成功");
    Check(future_cancel.value->status == TimingTaskStatus::kActive, "future 取消不应终止整个任务");
    Check(future_cancel.value->affected_instance_count == 2, "future 取消应统计边界及之后的实例");
    const auto task_after_future_cancel = future_cancel_store.FindTask(future_cancel_registered.value->task_id);
    Check(task_after_future_cancel.ok() && task_after_future_cancel.value->effective_until == 1785920399,
          "future 取消应保存排他的未来规则上界");
    const auto instances_after_future_cancel =
        future_cancel_store.ListInstances(future_cancel_registered.value->task_id);
    Check(instances_after_future_cancel.ok() && instances_after_future_cancel.value->size() == 3,
          "future 取消不应删除边界前实例");
    Check(instances_after_future_cancel.ok() &&
              instances_after_future_cancel.value->at(0).status == TimerInstanceStatus::kPending,
          "future 取消不应影响边界前 occurrence");
    Check(instances_after_future_cancel.ok() &&
              instances_after_future_cancel.value->at(1).status == TimerInstanceStatus::kSkipped &&
              instances_after_future_cancel.value->at(2).status == TimerInstanceStatus::kSkipped,
          "future 取消应跳过边界及之后 occurrence");

    InMemoryTimingTaskStore invalid_future_boundary_store;
    FixedTimingIdGenerator invalid_future_boundary_ids;
    DefaultTimingTaskService invalid_future_boundary_service(invalid_future_boundary_store, clock,
                                                             invalid_future_boundary_ids);
    const auto invalid_future_boundary_registered = invalid_future_boundary_service.RegisterTimerTask({
        .request_id = "request-invalid-future-boundary",
        .schedule_id = "schedule-invalid-future-boundary",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(invalid_future_boundary_registered.ok(), "非法 future 边界用例应先注册周期任务");
    const auto invalid_future_boundary = invalid_future_boundary_service.CancelTimerTask({
        .task_id = invalid_future_boundary_registered.value->task_id,
        .schedule_id = "schedule-invalid-future-boundary",
        .change_scope = ChangeScope::kFuture,
        .effective_from = 1785833999,
    });
    Check(invalid_future_boundary.status.code == ErrorCode::kInvalidArgument,
          "早于任务开始时间的 future 边界应返回参数错误");

    InMemoryTimingTaskStore future_terminal_instance_store;
    FixedTimingIdGenerator future_terminal_instance_ids;
    DefaultTimingTaskService future_terminal_instance_service(future_terminal_instance_store, clock,
                                                              future_terminal_instance_ids);
    const auto future_terminal_instance_registered = future_terminal_instance_service.RegisterTimerTask({
        .request_id = "request-future-terminal-instance",
        .schedule_id = "schedule-future-terminal-instance",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(future_terminal_instance_registered.ok(), "终态实例 future 取消用例应先注册周期任务");
    const auto future_terminal_instance_task =
        future_terminal_instance_store.FindTask(future_terminal_instance_registered.value->task_id);
    Check(future_terminal_instance_task.ok(), "终态实例 future 取消用例应能读回任务");
    TimingTask task_with_future_next_trigger = *future_terminal_instance_task.value;
    task_with_future_next_trigger.next_trigger_at = 1785920400;
    Check(future_terminal_instance_store
              .UpdateTaskWithInstances({
                  .task = task_with_future_next_trigger,
                  .upsert_instances =
                      {
                          TimerInstance{
                              .id = "instance-completed-at-future-boundary",
                              .task_id = future_terminal_instance_registered.value->task_id,
                              .planned_at = 1785920400,
                              .status = TimerInstanceStatus::kCompleted,
                          },
                          TimerInstance{
                              .id = "instance-pending-after-future-boundary",
                              .task_id = future_terminal_instance_registered.value->task_id,
                              .planned_at = 1786006800,
                              .status = TimerInstanceStatus::kPending,
                          },
                      },
              })
              .ok(),
          "终态实例 future 取消用例应能预置任务与实例");
    const auto future_terminal_instance_cancel = future_terminal_instance_service.CancelTimerTask({
        .task_id = future_terminal_instance_registered.value->task_id,
        .schedule_id = "schedule-future-terminal-instance",
        .change_scope = ChangeScope::kFuture,
        .effective_from = 1785920400,
    });
    Check(future_terminal_instance_cancel.ok(), "终态实例不应阻断 future 取消");
    Check(future_terminal_instance_cancel.value->affected_instance_count == 1,
          "future 取消只应统计实际转为 skipped 的实例");
    const auto task_after_future_terminal_instance_cancel =
        future_terminal_instance_store.FindTask(future_terminal_instance_registered.value->task_id);
    Check(task_after_future_terminal_instance_cancel.ok() &&
              task_after_future_terminal_instance_cancel.value->next_trigger_at == 0,
          "future 取消应清除位于取消边界的下一次触发");
    const auto instances_after_future_terminal_instance_cancel =
        future_terminal_instance_store.ListInstances(future_terminal_instance_registered.value->task_id);
    Check(instances_after_future_terminal_instance_cancel.ok() &&
              instances_after_future_terminal_instance_cancel.value->at(0).status == TimerInstanceStatus::kCompleted &&
              instances_after_future_terminal_instance_cancel.value->at(1).status == TimerInstanceStatus::kSkipped,
          "future 取消应保留 completed 实例并跳过仍待处理实例");

    InMemoryTimingTaskStore all_cancel_store;
    FixedTimingIdGenerator all_cancel_ids;
    DefaultTimingTaskService all_cancel_service(all_cancel_store, clock, all_cancel_ids);
    const auto all_cancel_registered = all_cancel_service.RegisterTimerTask({
        .request_id = "request-cancel-all",
        .schedule_id = "schedule-cancel-all",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(all_cancel_registered.ok(), "all 取消用例应先注册周期任务");
    const auto all_cancel_task = all_cancel_store.FindTask(all_cancel_registered.value->task_id);
    Check(all_cancel_task.ok(), "all 取消用例应能读回任务");
    Check(all_cancel_store
              .UpdateTaskWithInstances({
                  .task = *all_cancel_task.value,
                  .upsert_instances =
                      {
                          TimerInstance{
                              .id = "instance-first-all-cancel",
                              .task_id = all_cancel_registered.value->task_id,
                              .planned_at = 1785834000,
                              .status = TimerInstanceStatus::kPending,
                          },
                          TimerInstance{
                              .id = "instance-second-all-cancel",
                              .task_id = all_cancel_registered.value->task_id,
                              .planned_at = 1785920400,
                              .status = TimerInstanceStatus::kPending,
                          },
                      },
              })
              .ok(),
          "all 取消用例应能预置待取消实例");

    const auto all_cancel = all_cancel_service.CancelTimerTask({
        .task_id = all_cancel_registered.value->task_id,
        .schedule_id = "schedule-cancel-all",
        .change_scope = ChangeScope::kAll,
    });
    Check(all_cancel.ok(), "all 取消应成功");
    Check(all_cancel.value->status == TimingTaskStatus::kTerminated, "all 取消应终止任务");
    Check(all_cancel.value->affected_instance_count == 2, "all 取消应影响全部待处理实例");
    const auto task_after_all_cancel = all_cancel_store.FindTask(all_cancel_registered.value->task_id);
    Check(task_after_all_cancel.ok() && task_after_all_cancel.value->status == TimingTaskStatus::kTerminated,
          "all 取消后任务应进入 terminated");
    Check(task_after_all_cancel.ok() && task_after_all_cancel.value->next_trigger_at == 0,
          "all 取消后任务不应再产生未来触发");
    const auto instances_after_all_cancel = all_cancel_store.ListInstances(all_cancel_registered.value->task_id);
    Check(instances_after_all_cancel.ok() &&
              instances_after_all_cancel.value->at(0).status == TimerInstanceStatus::kSkipped &&
              instances_after_all_cancel.value->at(1).status == TimerInstanceStatus::kSkipped,
          "all 取消应跳过全部待处理 occurrence");

    const auto repeated_all_cancel = all_cancel_service.CancelTimerTask({
        .task_id = all_cancel_registered.value->task_id,
        .schedule_id = "schedule-cancel-all",
        .change_scope = ChangeScope::kAll,
    });
    Check(repeated_all_cancel.status.code == ErrorCode::kConflict, "重复 all 取消应返回稳定冲突错误");

    const auto missing_single_cancel = cancel_service.CancelTimerTask({
        .task_id = cancel_registered.value->task_id,
        .schedule_id = "schedule-cancel-single",
        .change_scope = ChangeScope::kSingle,
        .instance_id = "missing-instance",
        .target_occurrence_at = 1786006800,
    });
    Check(missing_single_cancel.status.code == ErrorCode::kNotFound, "不存在的 occurrence 应返回 not found");

    const auto repeated_single_cancel = cancel_service.CancelTimerTask({
        .task_id = cancel_registered.value->task_id,
        .schedule_id = "schedule-cancel-single",
        .change_scope = ChangeScope::kSingle,
        .instance_id = "instance-cancel-single",
        .target_occurrence_at = 1785920400,
    });
    Check(repeated_single_cancel.status.code == ErrorCode::kConflict, "重复 single 取消应返回稳定冲突错误");

    const auto invalid_future_cancel = future_cancel_service.CancelTimerTask({
        .task_id = future_cancel_registered.value->task_id,
        .schedule_id = "schedule-cancel-future",
        .change_scope = ChangeScope::kFuture,
    });
    Check(invalid_future_cancel.status.code == ErrorCode::kInvalidArgument,
          "缺少 effective_from 的 future 取消应返回参数错误");

    const auto invalid_scope_cancel = cancel_service.CancelTimerTask({
        .task_id = cancel_registered.value->task_id,
        .schedule_id = "schedule-cancel-single",
        .change_scope = static_cast<ChangeScope>(99),
    });
    Check(invalid_scope_cancel.status.code == ErrorCode::kInvalidArgument, "非法取消范围应返回参数错误");

    InMemoryTimingTaskStore failure_cancel_store;
    FixedTimingIdGenerator failure_cancel_ids;
    DefaultTimingTaskService failure_cancel_service(failure_cancel_store, clock, failure_cancel_ids);
    const auto failure_cancel_registered = failure_cancel_service.RegisterTimerTask({
        .request_id = "request-cancel-write-failure",
        .schedule_id = "schedule-cancel-write-failure",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    Check(failure_cancel_registered.ok(), "写入失败用例应先注册任务");
    const auto failure_cancel_task = failure_cancel_store.FindTask(failure_cancel_registered.value->task_id);
    Check(failure_cancel_task.ok(), "写入失败用例应能读回任务");
    Check(failure_cancel_store
              .UpdateTaskWithInstances({
                  .task = *failure_cancel_task.value,
                  .upsert_instances = {TimerInstance{
                      .id = "instance-cancel-write-failure",
                      .task_id = failure_cancel_registered.value->task_id,
                      .planned_at = 1785834000,
                      .status = TimerInstanceStatus::kPending,
                  }},
              })
              .ok(),
          "写入失败用例应能预置实例");
    failure_cancel_store.FailNextUpdate(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    const auto failed_single_cancel = failure_cancel_service.CancelTimerTask({
        .task_id = failure_cancel_registered.value->task_id,
        .schedule_id = "schedule-cancel-write-failure",
        .change_scope = ChangeScope::kSingle,
        .instance_id = "instance-cancel-write-failure",
        .target_occurrence_at = 1785834000,
    });
    Check(failed_single_cancel.status.code == ErrorCode::kUnavailable, "Store 写入失败应透传领域错误");
    const auto task_after_failed_cancel = failure_cancel_store.FindTask(failure_cancel_registered.value->task_id);
    Check(task_after_failed_cancel.ok() && task_after_failed_cancel.value->status == TimingTaskStatus::kActive,
          "Store 写入失败后任务状态不应半更新");
    const auto instances_after_failed_cancel =
        failure_cancel_store.ListInstances(failure_cancel_registered.value->task_id);
    Check(instances_after_failed_cancel.ok() &&
              instances_after_failed_cancel.value->front().status == TimerInstanceStatus::kPending,
          "Store 写入失败后实例状态不应半更新");
    return 0;
}
