#include "voicelife/timing/timing_task_service.h"

#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/contracts/status.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::ChangeScope;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::RecurrenceRule;
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
using voicelife::timing::TimingTaskUpdateWrite;

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

    Status UpdateTaskWithInstances(const TimingTaskUpdateWrite&) override {
        return Status::Error(ErrorCode::kInternal, "unexpected update");
    }

    Result<TimingTask> FindTask(const TimingTaskId&) override {
        return Result<TimingTask>::Success({
            .id = "task-lookup",
            .schedule_id = "schedule-lookup",
            .start_at = 1785834000,
            .next_trigger_at = 1785834000,
        });
    }

    Result<std::vector<ReminderRule>> ListRules(const TimingTaskId&) override {
        return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal, "unexpected list");
    }

    Result<std::vector<TimerInstance>> ListInstances(const TimingTaskId&) override {
        return Result<std::vector<TimerInstance>>::Failure(ErrorCode::kInternal, "unexpected list instances");
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

    Status UpdateTaskWithInstances(const TimingTaskUpdateWrite&) override {
        return Status::Error(ErrorCode::kInternal, "unexpected update");
    }

    Result<TimingTask> FindTask(const TimingTaskId&) override {
        return Result<TimingTask>::Failure(ErrorCode::kInternal, "unexpected find");
    }

    Result<std::vector<ReminderRule>> ListRules(const TimingTaskId&) override {
        return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal, "unexpected list");
    }

    Result<std::vector<TimerInstance>> ListInstances(const TimingTaskId&) override {
        return Result<std::vector<TimerInstance>>::Failure(ErrorCode::kInternal, "unexpected list instances");
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

    const auto update_lookup_failure = lookup_failure_service.UpdateTimerTask({
        .task_id = "task-lookup",
        .schedule_id = "schedule-lookup",
        .change_scope = ChangeScope::kAll,
        .start_at = 1785920400,
    });
    Check(update_lookup_failure.status.code == ErrorCode::kInternal, "实例查询失败时应返回 Store 错误");

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

    InMemoryTimingTaskStore single_store;
    FixedTimingIdGenerator single_ids;
    DefaultTimingTaskService single_service(single_store, clock, single_ids);
    const auto single_registered = single_service.RegisterTimerTask({
        .request_id = "request-single",
        .schedule_id = "schedule-single",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(single_registered.ok(), "single 修改用例应先注册周期任务");
    const auto single_task_before_update = single_store.FindTask(single_registered.value->task_id);
    Check(single_store
              .UpdateTaskWithInstances({
                  .task = *single_task_before_update.value,
                  .upsert_instances = {TimerInstance{
                      .id = "instance-single",
                      .task_id = single_registered.value->task_id,
                      .planned_at = 1785920400,
                      .status = TimerInstanceStatus::kPending,
                  }},
              })
              .ok(),
          "single 修改用例应能预置待覆盖实例");
    const auto single_update = single_service.UpdateTimerTask({
        .task_id = single_registered.value->task_id,
        .schedule_id = "schedule-single",
        .change_scope = ChangeScope::kSingle,
        .start_at = 1785924000,
        .instance_id = "instance-single",
        .target_occurrence_at = 1785920400,
    });
    Check(single_update.ok(), "single 修改应成功");
    Check(single_update.value->instance_id == "instance-single", "single 修改应返回被覆盖的实例");
    Check(single_update.value->affected_instance_count == 1, "single 修改只影响一个实例");
    Check(single_update.value->override_fields.start_at.has_value() &&
              *single_update.value->override_fields.start_at == 1785924000,
          "single 修改应返回该实例的新开始时间覆盖字段");
    const auto single_task = single_store.FindTask(single_registered.value->task_id);
    Check(single_task.ok() && single_task.value->start_at == 1785834000, "single 修改不应改变任务周期锚点");
    const auto single_instances = single_store.ListInstances(single_registered.value->task_id);
    Check(single_instances.ok() && single_instances.value->size() == 1, "single 修改只应产生一个实例覆盖");
    Check(single_instances.ok() && single_instances.value->front().id == "instance-single",
          "single 修改不应改写其他实例");
    Check(single_instances.ok() && single_instances.value->front().status == TimerInstanceStatus::kModified,
          "single 修改产生的实例应进入 modified 状态");

    const auto mismatched_single_target = single_service.UpdateTimerTask({
        .task_id = single_registered.value->task_id,
        .schedule_id = "schedule-single",
        .change_scope = ChangeScope::kSingle,
        .start_at = 1786006800,
        .instance_id = "instance-single",
        .target_occurrence_at = 1786003200,
    });
    Check(mismatched_single_target.status.code == ErrorCode::kConflict,
          "single 修改不应将已有实例重定向到其他 occurrence");

    Check(single_store
              .UpdateTaskWithInstances({
                  .task = *single_task.value,
                  .upsert_instances = {TimerInstance{
                      .id = "instance-completed",
                      .task_id = single_registered.value->task_id,
                      .planned_at = 1786003200,
                      .status = TimerInstanceStatus::kCompleted,
                  }},
              })
              .ok(),
          "single 修改用例应能预置终态实例");
    const auto completed_single_target = single_service.UpdateTimerTask({
        .task_id = single_registered.value->task_id,
        .schedule_id = "schedule-single",
        .change_scope = ChangeScope::kSingle,
        .start_at = 1786006800,
        .instance_id = "instance-completed",
        .target_occurrence_at = 1786003200,
    });
    Check(completed_single_target.status.code == ErrorCode::kConflict, "single 修改不应重新打开已完成 occurrence");

    const auto single_recurrence_update = single_service.UpdateTimerTask({
        .task_id = single_registered.value->task_id,
        .schedule_id = "schedule-single",
        .change_scope = ChangeScope::kSingle,
        .start_at = 1785924000,
        .instance_id = "instance-single-2",
        .target_occurrence_at = 1785920400,
        .recurrence = RecurrenceRule{.frequency = RecurrenceFrequency::kDay},
    });
    Check(single_recurrence_update.status.code == ErrorCode::kInvalidArgument, "single 修改不应接受新的周期规则");

    const auto schedule_conflict = single_service.UpdateTimerTask({
        .task_id = single_registered.value->task_id,
        .schedule_id = "schedule-other",
        .change_scope = ChangeScope::kAll,
        .start_at = 1785924000,
    });
    Check(schedule_conflict.status.code == ErrorCode::kConflict, "修改任务必须匹配所属日程");

    InMemoryTimingTaskStore future_store;
    FixedTimingIdGenerator future_ids;
    DefaultTimingTaskService future_service(future_store, clock, future_ids);
    const auto future_registered = future_service.RegisterTimerTask({
        .request_id = "request-future",
        .schedule_id = "schedule-future",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(future_registered.ok(), "future 修改用例应先注册周期任务");
    const auto future_task = future_store.FindTask(future_registered.value->task_id);
    Check(future_task.ok(), "future 修改用例应能读回任务");
    const TimerInstance before_boundary{
        .id = "instance-before-boundary",
        .task_id = future_registered.value->task_id,
        .planned_at = 1785834000,
        .status = TimerInstanceStatus::kPending,
    };
    const TimerInstance at_boundary{
        .id = "instance-at-boundary",
        .task_id = future_registered.value->task_id,
        .planned_at = 1785920400,
        .status = TimerInstanceStatus::kPending,
    };
    Check(future_store
              .UpdateTaskWithInstances({
                  .task = *future_task.value,
                  .upsert_instances = {before_boundary, at_boundary},
              })
              .ok(),
          "future 修改用例应能预置边界前后的实例");
    const auto future_update = future_service.UpdateTimerTask({
        .task_id = future_registered.value->task_id,
        .schedule_id = "schedule-future",
        .change_scope = ChangeScope::kFuture,
        .start_at = 1786006800,
        .effective_from = 1785920400,
        .recurrence = RecurrenceRule{.frequency = RecurrenceFrequency::kWeek, .by_weekdays = {2}},
    });
    Check(future_update.ok(), "future 修改应成功");
    Check(future_update.value->affected_instance_count == 1, "future 修改只统计生效边界及之后的已物化实例");
    const auto future_after = future_store.FindTask(future_registered.value->task_id);
    Check(future_after.ok() && future_after.value->start_at == 1786006800, "future 修改应更新任务未来周期锚点");
    Check(future_after.ok() && future_after.value->next_trigger_at == 1786006800,
          "future 修改应重算下一次未来触发时间");
    Check(future_after.ok() && future_after.value->recurrence.frequency == RecurrenceFrequency::kWeek,
          "future 修改应保存新的未来周期规则");
    const auto future_instances = future_store.ListInstances(future_registered.value->task_id);
    Check(future_instances.ok() && future_instances.value->front().id == "instance-before-boundary",
          "future 修改后边界前实例仍应存在");
    Check(future_instances.ok() && future_instances.value->front().planned_at == 1785834000,
          "future 修改不应改变生效边界之前的 occurrence");
    Check(future_instances.ok() && future_instances.value->at(1).status == TimerInstanceStatus::kSkipped,
          "future 修改应作废生效边界及之后的旧 occurrence");

    InMemoryTimingTaskStore all_store;
    FixedTimingIdGenerator all_ids;
    DefaultTimingTaskService all_service(all_store, clock, all_ids);
    const auto all_registered = all_service.RegisterTimerTask({
        .request_id = "request-all",
        .schedule_id = "schedule-all",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(all_registered.ok(), "all 修改用例应先注册周期任务");
    const auto all_task_before_update = all_store.FindTask(all_registered.value->task_id);
    Check(all_store
              .UpdateTaskWithInstances({
                  .task = *all_task_before_update.value,
                  .upsert_instances = {TimerInstance{
                      .id = "instance-all",
                      .task_id = all_registered.value->task_id,
                      .planned_at = 1785920400,
                      .status = TimerInstanceStatus::kPending,
                  }},
              })
              .ok(),
          "all 修改用例应能预置旧 occurrence");
    const auto all_update = all_service.UpdateTimerTask({
        .task_id = all_registered.value->task_id,
        .schedule_id = "schedule-all",
        .change_scope = ChangeScope::kAll,
        .start_at = 1786093200,
        .recurrence = RecurrenceRule{.frequency = RecurrenceFrequency::kMonth, .by_month_days = {5}},
    });
    Check(all_update.ok(), "all 修改应成功");
    Check(all_update.value->affected_instance_count == 1, "all 修改应统计被作废的已物化实例");
    Check(all_update.value->next_trigger_at == 1786093200, "all 修改应返回重算后的下一次触发时间");
    const auto all_task = all_store.FindTask(all_registered.value->task_id);
    Check(all_task.ok() && all_task.value->start_at == 1786093200, "all 修改应更新任务周期锚点");
    Check(all_task.ok() && all_task.value->next_trigger_at == 1786093200, "all 修改应让任务下一次触发时间与新锚点一致");
    Check(all_task.ok() && all_task.value->recurrence.frequency == RecurrenceFrequency::kMonth,
          "all 修改应保存新的任务规则");
    const auto all_instances = all_store.ListInstances(all_registered.value->task_id);
    Check(all_instances.ok() && all_instances.value->front().status == TimerInstanceStatus::kSkipped,
          "all 修改应作废旧 occurrence");

    auto terminated_task = *all_task.value;
    terminated_task.status = TimingTaskStatus::kTerminated;
    Check(all_store.UpdateTaskWithInstances({.task = terminated_task}).ok(), "测试应能预置已终止任务");
    const auto terminated_update = all_service.UpdateTimerTask({
        .task_id = all_registered.value->task_id,
        .schedule_id = "schedule-all",
        .change_scope = ChangeScope::kAll,
        .start_at = 1786179600,
    });
    Check(terminated_update.status.code == ErrorCode::kConflict, "已终止任务不应再次修改");

    const auto illegal_scope = all_service.UpdateTimerTask({
        .task_id = all_registered.value->task_id,
        .schedule_id = "schedule-all",
        .change_scope = static_cast<ChangeScope>(99),
        .start_at = 1786093200,
    });
    Check(illegal_scope.status.code == ErrorCode::kInvalidArgument, "非法修改范围应返回参数错误");

    const auto missing_single_target = all_service.UpdateTimerTask({
        .task_id = all_registered.value->task_id,
        .schedule_id = "schedule-all",
        .change_scope = ChangeScope::kSingle,
        .start_at = 1786093200,
    });
    Check(missing_single_target.status.code == ErrorCode::kInvalidArgument,
          "single 修改缺少目标 occurrence 应返回参数错误");

    const auto missing_future_target = all_service.UpdateTimerTask({
        .task_id = all_registered.value->task_id,
        .schedule_id = "schedule-all",
        .change_scope = ChangeScope::kFuture,
        .start_at = 1786093200,
    });
    Check(missing_future_target.status.code == ErrorCode::kInvalidArgument,
          "future 修改缺少 effective_from 应返回参数错误");

    const auto missing_identity = all_service.UpdateTimerTask({
        .change_scope = ChangeScope::kAll,
        .start_at = 1786093200,
    });
    Check(missing_identity.status.code == ErrorCode::kInvalidArgument, "修改任务缺少标识时应返回参数错误");

    const auto missing_start = all_service.UpdateTimerTask({
        .task_id = all_registered.value->task_id,
        .schedule_id = "schedule-all",
        .change_scope = ChangeScope::kAll,
    });
    Check(missing_start.status.code == ErrorCode::kInvalidArgument, "修改任务缺少开始时间时应返回参数错误");

    const auto not_found = all_service.UpdateTimerTask({
        .task_id = "task-missing",
        .schedule_id = "schedule-all",
        .change_scope = ChangeScope::kAll,
        .start_at = 1786093200,
    });
    Check(not_found.status.code == ErrorCode::kNotFound, "不存在的任务应返回 not found");

    InMemoryTimingTaskStore failing_store;
    FixedTimingIdGenerator failing_ids;
    DefaultTimingTaskService failing_service(failing_store, clock, failing_ids);
    const auto failing_registered = failing_service.RegisterTimerTask({
        .request_id = "request-failing",
        .schedule_id = "schedule-failing",
        .start_at = 1785834000,
        .time_zone = "Asia/Shanghai",
    });
    Check(failing_registered.ok(), "失败路径用例应先注册任务");
    const auto failing_task_before_update = failing_store.FindTask(failing_registered.value->task_id);
    Check(failing_store
              .UpdateTaskWithInstances({
                  .task = *failing_task_before_update.value,
                  .upsert_instances = {TimerInstance{
                      .id = "instance-failing",
                      .task_id = failing_registered.value->task_id,
                      .planned_at = 1785920400,
                      .status = TimerInstanceStatus::kPending,
                  }},
              })
              .ok(),
          "失败路径用例应能预置旧 occurrence");
    failing_store.FailNextUpdate(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    const auto failing_update = failing_service.UpdateTimerTask({
        .task_id = failing_registered.value->task_id,
        .schedule_id = "schedule-failing",
        .change_scope = ChangeScope::kAll,
        .start_at = 1786093200,
    });
    Check(failing_update.status.code == ErrorCode::kUnavailable, "Store 写入失败应向调用方返回存储错误");
    const auto unchanged_after_failure = failing_store.FindTask(failing_registered.value->task_id);
    Check(unchanged_after_failure.ok() && unchanged_after_failure.value->start_at == 1785834000,
          "Store 写入失败后任务不应半更新");
    const auto instances_after_failure = failing_store.ListInstances(failing_registered.value->task_id);
    Check(instances_after_failure.ok() && instances_after_failure.value->size() == 1,
          "Store 写入失败后不应新增或删除实例");
    Check(
        instances_after_failure.ok() && instances_after_failure.value->front().status == TimerInstanceStatus::kPending,
        "Store 写入失败后旧 occurrence 不应被半更新");
    return 0;
}
