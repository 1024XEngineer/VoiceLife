#include "voicelife/timing/timing_task_service.h"

#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/contracts/status.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::CalendarSortBy;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::RegisterTimerTaskCommand;
using voicelife::timing::ReminderRule;
using voicelife::timing::ReminderRuleInput;
using voicelife::timing::ReminderType;
using voicelife::timing::SortOrder;
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

    Result<TimingTask> FindTask(const TimingTaskId&) override {
        return Result<TimingTask>::Failure(ErrorCode::kInternal, "unexpected find");
    }

    Result<std::vector<TimingTask>> ListTasks() override {
        return Result<std::vector<TimingTask>>::Failure(ErrorCode::kInternal, "unexpected list tasks");
    }

    Result<std::vector<ReminderRule>> ListRules(const TimingTaskId&) override {
        return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal, "unexpected list");
    }

    Result<std::vector<voicelife::timing::TimerInstance>> ListInstances(const TimingTaskId&) override {
        return Result<std::vector<voicelife::timing::TimerInstance>>::Failure(ErrorCode::kInternal,
                                                                              "unexpected list instances");
    }

    Status UpsertRules(const TimingTaskId&, const std::vector<ReminderRule>&) override {
        return Status::Error(ErrorCode::kInternal, "unexpected upsert");
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

    Result<std::vector<TimingTask>> ListTasks() override {
        return Result<std::vector<TimingTask>>::Failure(ErrorCode::kInternal, "unexpected list tasks");
    }

    Result<std::vector<ReminderRule>> ListRules(const TimingTaskId&) override {
        return Result<std::vector<ReminderRule>>::Failure(ErrorCode::kInternal, "unexpected list");
    }

    Result<std::vector<voicelife::timing::TimerInstance>> ListInstances(const TimingTaskId&) override {
        return Result<std::vector<voicelife::timing::TimerInstance>>::Failure(ErrorCode::kInternal,
                                                                              "unexpected list instances");
    }

    Status UpsertRules(const TimingTaskId&, const std::vector<ReminderRule>&) override {
        return Status::Error(ErrorCode::kInternal, "unexpected upsert");
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

    const auto calendar = service.ListCalendarView({
        .range_start = 1785740000,
        .range_end = 1785750000,
        .schedule_id = "schedule-1",
        .page = 1,
        .page_size = 20,
        .sort_by = CalendarSortBy::kPlannedStartAt,
    });
    Check(calendar.ok(), "有效范围应返回日历视图");
    Check(calendar.value->total == 1 && calendar.value->occurrences.size() == 1, "一次性任务应返回范围内 occurrence");
    Check(calendar.value->occurrences.front().task_id == registered.value->task_id, "日历 occurrence 应关联原任务");
    Check(calendar.value->occurrences.front().planned_start_at == 1785747600, "日历 occurrence 应使用任务开始时间");

    InMemoryTimingTaskStore recurring_calendar_store;
    FixedTimingIdGenerator recurring_calendar_ids;
    DefaultTimingTaskService recurring_calendar_service(recurring_calendar_store, clock, recurring_calendar_ids);
    const auto recurring_calendar_task = recurring_calendar_service.RegisterTimerTask({
        .request_id = "request-calendar-recurring",
        .schedule_id = "schedule-calendar-recurring",
        .start_at = 1785747600,
        .time_zone = "UTC",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(recurring_calendar_task.ok(), "周期日历任务应注册成功");
    constexpr int64_t kDay = 24 * 60 * 60;
    recurring_calendar_store.AddInstance({
        .id = "instance-calendar-modified",
        .task_id = recurring_calendar_task.value->task_id,
        .planned_at = 1785747600 + kDay,
        .planned_end_at = 1785747600 + kDay + 1800,
        .status = TimerInstanceStatus::kModified,
        .override_fields =
            {
                .start_at = 1785747600 + kDay + 3600,
                .end_at = 1785747600 + kDay + 5400,
            },
    });

    const auto recurring_calendar = recurring_calendar_service.ListCalendarView({
        .range_start = 1785747600,
        .range_end = 1785747600 + 3 * kDay,
        .schedule_id = "schedule-calendar-recurring",
        .page = 1,
        .page_size = 2,
        .sort_by = CalendarSortBy::kPlannedStartAt,
        .sort_order = SortOrder::kAscending,
    });
    Check(recurring_calendar.ok() && recurring_calendar.value->total == 3,
          "未物化的每日规则和单次例外应合并为三条 occurrence");
    Check(recurring_calendar.value->occurrences.size() == 2 && recurring_calendar.value->has_more,
          "日历视图应按 page_size 返回第一页并标记后续页");
    Check(recurring_calendar.value->occurrences[1].is_exception &&
              recurring_calendar.value->occurrences[1].planned_start_at == 1785747600 + kDay + 3600,
          "已修改例外应覆盖基础 occurrence 的开始时间");

    const auto pending_only = recurring_calendar_service.ListCalendarView({
        .range_start = 1785747600,
        .range_end = 1785747600 + 3 * kDay,
        .schedule_id = "schedule-calendar-recurring",
        .status = TimerInstanceStatus::kPending,
    });
    Check(pending_only.ok() && pending_only.value->total == 2, "状态过滤应排除已修改例外");

    const auto invalid_calendar = service.ListCalendarView({.range_start = 10, .range_end = 10});
    Check(invalid_calendar.status.code == ErrorCode::kInvalidArgument, "左闭右开范围必须满足 start 小于 end");

    const auto missing_schedule = service.ListCalendarView({
        .range_start = 1785740000,
        .range_end = 1785750000,
        .schedule_id = "missing-schedule",
    });
    Check(missing_schedule.ok() && missing_schedule.value->total == 0, "不存在的日程过滤目标应返回稳定空结果");

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

    const auto empty_rule_request = service.UpsertReminderRules({});
    Check(empty_rule_request.status.code == ErrorCode::kInvalidArgument, "空提醒规则请求应返回参数错误");

    const auto wrong_schedule = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "other-schedule",
        .rules =
            {
                ReminderRuleInput{
                    .type = ReminderType::kWeak,
                    .offset_minutes = -30,
                    .channel = "voice",
                },
            },
    });
    Check(wrong_schedule.status.code == ErrorCode::kConflict, "任务不属于指定日程应返回冲突错误");

    const auto upserted_rules = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .type = ReminderType::kWeak,
                    .offset_minutes = -30,
                    .channel = "voice",
                },
                ReminderRuleInput{
                    .type = ReminderType::kStrong,
                    .offset_minutes = -5,
                    .max_snooze_count = 2,
                    .snooze_interval_minutes = 5,
                    .channel = "voice",
                },
            },
    });
    Check(upserted_rules.ok(), "合法 weak/strong 规则应创建成功");
    Check(upserted_rules.value->reminder_rules.size() == 4, "创建规则后应返回任务的完整规则列表");
    std::string created_weak_rule_id;
    std::string created_strong_rule_id;
    for (const auto& rule : upserted_rules.value->reminder_rules) {
        if (rule.type == ReminderType::kWeak && rule.offset_minutes == -30) {
            created_weak_rule_id = rule.id;
        }
        if (rule.type == ReminderType::kStrong && rule.offset_minutes == -5) {
            created_strong_rule_id = rule.id;
        }
    }
    Check(!created_weak_rule_id.empty(), "新弱提醒规则应获得服务端标识");
    Check(!created_strong_rule_id.empty() && created_strong_rule_id != created_weak_rule_id,
          "每条新规则应获得不同的稳定标识");

    const auto updated_rule = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .reminder_rule_id = created_weak_rule_id,
                    .type = ReminderType::kWeak,
                    .offset_minutes = -20,
                    .channel = "voice",
                },
            },
    });
    Check(updated_rule.ok(), "已有提醒规则应更新成功");
    Check(updated_rule.value->reminder_rules.size() == 4, "更新已有规则不能创建重复规则");
    bool has_updated_rule = false;
    for (const auto& rule : updated_rule.value->reminder_rules) {
        has_updated_rule = has_updated_rule || (rule.id == created_weak_rule_id && rule.offset_minutes == -20);
    }
    Check(has_updated_rule, "更新已有规则应保留原规则标识");

    const auto duplicate_rule_update = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .reminder_rule_id = created_weak_rule_id,
                    .type = ReminderType::kWeak,
                    .offset_minutes = -20,
                    .channel = "voice",
                },
                ReminderRuleInput{
                    .reminder_rule_id = created_weak_rule_id,
                    .type = ReminderType::kWeak,
                    .offset_minutes = -25,
                    .channel = "voice",
                },
            },
    });
    Check(duplicate_rule_update.status.code == ErrorCode::kConflict, "同一请求重复规则标识应返回冲突错误");

    store.FailNextRuleList(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    const auto list_failure = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .type = ReminderType::kWeak,
                    .offset_minutes = -15,
                    .channel = "voice",
                },
            },
    });
    Check(list_failure.status.code == ErrorCode::kUnavailable, "规则读取失败应透传 Store 错误");

    const auto future_offset = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .type = ReminderType::kWeak,
                    .offset_minutes = 1,
                    .channel = "voice",
                },
            },
    });
    Check(future_offset.status.code == ErrorCode::kInvalidArgument, "提醒规则不能晚于任务开始时间触发");

    const auto weak_snooze = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .type = ReminderType::kWeak,
                    .offset_minutes = -15,
                    .max_snooze_count = 1,
                    .snooze_interval_minutes = 5,
                    .channel = "voice",
                },
            },
    });
    Check(weak_snooze.status.code == ErrorCode::kInvalidArgument, "弱提醒配置 snooze 应返回参数错误");

    const auto invalid_strong_snooze = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .type = ReminderType::kStrong,
                    .offset_minutes = -15,
                    .max_snooze_count = 0,
                    .snooze_interval_minutes = 5,
                    .channel = "voice",
                },
            },
    });
    Check(invalid_strong_snooze.status.code == ErrorCode::kInvalidArgument, "强提醒必须配置正数 snooze 次数和间隔");

    const auto duplicate_on_time_strong = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .type = ReminderType::kStrong,
                    .offset_minutes = 0,
                    .max_snooze_count = 3,
                    .snooze_interval_minutes = 5,
                    .channel = "voice",
                },
            },
    });
    Check(duplicate_on_time_strong.status.code == ErrorCode::kConflict, "同一任务的第二条准点强提醒应返回冲突错误");
    const auto rules_after_conflict = store.ListRules(registered.value->task_id);
    Check(rules_after_conflict.ok() && rules_after_conflict.value->size() == 4, "规则冲突后不能留下部分创建结果");

    const auto unknown_task = service.UpsertReminderRules({
        .task_id = "missing-task",
        .schedule_id = "missing-schedule",
        .rules =
            {
                ReminderRuleInput{
                    .type = ReminderType::kWeak,
                    .offset_minutes = -15,
                    .channel = "voice",
                },
            },
    });
    Check(unknown_task.status.code == ErrorCode::kNotFound, "未知任务应返回 not found");

    const auto unknown_rule = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .reminder_rule_id = "missing-rule",
                    .type = ReminderType::kWeak,
                    .offset_minutes = -15,
                    .channel = "voice",
                },
            },
    });
    Check(unknown_rule.status.code == ErrorCode::kNotFound, "未知规则应返回 not found");

    store.FailNextRuleUpsert(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    const auto failed_update = service.UpsertReminderRules({
        .task_id = registered.value->task_id,
        .schedule_id = "schedule-1",
        .rules =
            {
                ReminderRuleInput{
                    .reminder_rule_id = created_weak_rule_id,
                    .type = ReminderType::kWeak,
                    .offset_minutes = -10,
                    .channel = "voice",
                },
            },
    });
    Check(failed_update.status.code == ErrorCode::kUnavailable, "规则写入失败应透传 Store 错误");
    const auto rules_after_failed_update = store.ListRules(registered.value->task_id);
    bool retained_previous_rule = false;
    for (const auto& rule : *rules_after_failed_update.value) {
        retained_previous_rule =
            retained_previous_rule || (rule.id == created_weak_rule_id && rule.offset_minutes == -20);
    }
    Check(retained_previous_rule, "规则写入失败不能改变已保存规则");
    return 0;
}
