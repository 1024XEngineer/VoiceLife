#include <limits>
#include <string>

#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/timing/timing_task_service.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;
using voicelife::test::InMemoryTimingTaskStore;
using voicelife::timing::CalendarSortBy;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::SortOrder;
using voicelife::timing::TimerInstanceStatus;
using voicelife::timing::TimingClockPort;
using voicelife::timing::TimingIdGeneratorPort;
using voicelife::timing::TimingTaskStatus;

namespace {

constexpr int64_t kStartAt = 1785747600;
constexpr int64_t kDay = 24 * 60 * 60;

class FixedTimingClock final : public TimingClockPort {
   public:
    int64_t Now() const override { return kStartAt - 3600; }
};

class FixedTimingIdGenerator final : public TimingIdGeneratorPort {
   public:
    std::string NextTaskId() override { return "task-" + std::to_string(next_task_++); }
    std::string NextReminderRuleId() override { return "rule-" + std::to_string(next_rule_++); }

   private:
    int next_task_ = 1;
    int next_rule_ = 1;
};

}  // namespace

int main() {
    FixedTimingClock clock;

    InMemoryTimingTaskStore single_store;
    FixedTimingIdGenerator single_ids;
    DefaultTimingTaskService single_service(single_store, clock, single_ids);
    const auto single_task = single_service.RegisterTimerTask({
        .request_id = "calendar-single",
        .schedule_id = "schedule-single",
        .start_at = kStartAt,
        .time_zone = "Asia/Shanghai",
    });
    Check(single_task.ok(), "一次性日程应注册成功");
    const auto single_calendar = single_service.ListCalendarView({
        .range_start = kStartAt - 1,
        .range_end = kStartAt + 1,
        .schedule_id = "schedule-single",
    });
    Check(single_calendar.ok() && single_calendar.value->total == 1 &&
              single_calendar.value->occurrences.front().task_id == single_task.value->task_id,
          "一次性任务应按范围返回 occurrence");

    InMemoryTimingTaskStore calendar_store;
    FixedTimingIdGenerator calendar_ids;
    DefaultTimingTaskService calendar_service(calendar_store, clock, calendar_ids);
    const auto recurring_task = calendar_service.RegisterTimerTask({
        .request_id = "calendar-daily",
        .schedule_id = "schedule-daily",
        .start_at = kStartAt,
        .time_zone = "UTC",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
    });
    Check(recurring_task.ok(), "每日任务应注册成功");
    calendar_store.AddInstance({
        .id = "calendar-modified",
        .task_id = recurring_task.value->task_id,
        .planned_at = kStartAt + kDay,
        .planned_end_at = kStartAt + kDay + 1800,
        .actual_trigger_at = kStartAt + kDay + 300,
        .status = TimerInstanceStatus::kModified,
        .override_fields = {.start_at = kStartAt + kDay + 3600, .end_at = kStartAt + kDay + 5400},
    });
    const auto recurring_calendar = calendar_service.ListCalendarView({
        .range_start = kStartAt,
        .range_end = kStartAt + 3 * kDay,
        .schedule_id = "schedule-daily",
        .page = 1,
        .page_size = 2,
    });
    Check(recurring_calendar.ok() && recurring_calendar.value->total == 3 &&
              recurring_calendar.value->occurrences.size() == 2 && recurring_calendar.value->has_more &&
              recurring_calendar.value->occurrences[1].is_exception &&
              recurring_calendar.value->occurrences[1].planned_start_at == kStartAt + kDay + 3600,
          "基础每日规则和已修改例外应合并并分页");

    const auto descending_page = calendar_service.ListCalendarView({
        .range_start = kStartAt,
        .range_end = kStartAt + 3 * kDay,
        .schedule_id = "schedule-daily",
        .page = 2,
        .page_size = 2,
        .sort_order = SortOrder::kDescending,
    });
    Check(descending_page.ok() && descending_page.value->occurrences.size() == 1 &&
              descending_page.value->occurrences.front().planned_start_at == kStartAt,
          "降序第二页应返回最早 occurrence");
    const auto actual_sort = calendar_service.ListCalendarView({
        .range_start = kStartAt,
        .range_end = kStartAt + 3 * kDay,
        .schedule_id = "schedule-daily",
        .sort_by = CalendarSortBy::kActualTriggerAt,
        .sort_order = SortOrder::kDescending,
    });
    Check(actual_sort.ok() && actual_sort.value->occurrences.front().is_exception, "实际触发时间降序应优先已触发例外");
    const auto pending_only = calendar_service.ListCalendarView({
        .range_start = kStartAt,
        .range_end = kStartAt + 3 * kDay,
        .schedule_id = "schedule-daily",
        .status = TimerInstanceStatus::kPending,
    });
    Check(pending_only.ok() && pending_only.value->total == 2, "状态过滤应排除已修改例外");

    calendar_store.FailNextTaskList(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    Check(calendar_service.ListCalendarView({.range_start = kStartAt, .range_end = kStartAt + kDay}).status.code ==
              ErrorCode::kUnavailable,
          "任务查询失败应透传 Store 错误");
    calendar_store.FailNextInstanceList(Status::Error(ErrorCode::kUnavailable, "store unavailable"));
    Check(calendar_service.ListCalendarView({.range_start = kStartAt, .range_end = kStartAt + kDay}).status.code ==
              ErrorCode::kUnavailable,
          "实例查询失败应透传 Store 错误");

    Check(single_service.ListCalendarView({.range_start = 10, .range_end = 10}).status.code ==
              ErrorCode::kInvalidArgument,
          "空范围应返回参数错误");
    Check(single_service
                  .ListCalendarView({
                      .range_start = std::numeric_limits<int64_t>::min(),
                      .range_end = std::numeric_limits<int64_t>::min() + kDay,
                  })
                  .status.code == ErrorCode::kInvalidArgument,
          "无法安全对齐日边界的极小时间戳应被拒绝");

    calendar_store.AddTask({
        .id = "calendar-deleted",
        .schedule_id = "schedule-deleted",
        .start_at = kStartAt,
        .time_zone = "UTC",
        .status = TimingTaskStatus::kActive,
        .deleted_at = kStartAt,
    });
    Check(calendar_service
                  .ListCalendarView({
                      .range_start = kStartAt,
                      .range_end = kStartAt + kDay,
                      .schedule_id = "schedule-deleted",
                  })
                  .value->total == 0,
          "软删除任务不应出现在日历视图中");

    InMemoryTimingTaskStore weekly_store;
    FixedTimingIdGenerator weekly_ids;
    DefaultTimingTaskService weekly_service(weekly_store, clock, weekly_ids);
    Check(weekly_service
              .RegisterTimerTask({
                  .request_id = "calendar-weekly",
                  .schedule_id = "schedule-weekly",
                  .start_at = kStartAt,
                  .time_zone = "UTC",
                  .recurrence = {.frequency = RecurrenceFrequency::kWeek},
              })
              .ok(),
          "每周任务应注册成功");
    Check(
        weekly_service.ListCalendarView({.range_start = kStartAt, .range_end = kStartAt + 15 * kDay}).value->total == 3,
        "每周规则应按七天间隔展开");

    InMemoryTimingTaskStore monthly_store;
    FixedTimingIdGenerator monthly_ids;
    DefaultTimingTaskService monthly_service(monthly_store, clock, monthly_ids);
    Check(monthly_service
              .RegisterTimerTask({
                  .request_id = "calendar-monthly",
                  .schedule_id = "schedule-monthly",
                  .start_at = kStartAt,
                  .time_zone = "UTC",
                  .recurrence = {.frequency = RecurrenceFrequency::kMonth},
              })
              .ok(),
          "每月任务应注册成功");
    Check(monthly_service.ListCalendarView({.range_start = kStartAt, .range_end = kStartAt + 70 * kDay}).value->total ==
              3,
          "每月规则应按周期锚点的日期展开");

    InMemoryTimingTaskStore yearly_store;
    FixedTimingIdGenerator yearly_ids;
    DefaultTimingTaskService yearly_service(yearly_store, clock, yearly_ids);
    Check(yearly_service
              .RegisterTimerTask({
                  .request_id = "calendar-yearly",
                  .schedule_id = "schedule-yearly",
                  .start_at = kStartAt,
                  .time_zone = "UTC",
                  .recurrence = {.frequency = RecurrenceFrequency::kYear},
              })
              .ok(),
          "每年任务应注册成功");
    Check(yearly_service.ListCalendarView({.range_start = kStartAt, .range_end = kStartAt + 370 * kDay}).value->total ==
              2,
          "每年规则应按周期锚点的月日展开");

    InMemoryTimingTaskStore timezone_store;
    FixedTimingIdGenerator timezone_ids;
    DefaultTimingTaskService timezone_service(timezone_store, clock, timezone_ids);
    Check(timezone_service
              .RegisterTimerTask({
                  .request_id = "calendar-local-zone",
                  .schedule_id = "schedule-local-zone",
                  .start_at = kStartAt,
                  .time_zone = "Asia/Shanghai",
                  .recurrence = {.frequency = RecurrenceFrequency::kWeek},
              })
              .ok(),
          "非 UTC 周期任务仍可注册");
    Check(timezone_service.ListCalendarView({.range_start = kStartAt, .range_end = kStartAt + kDay}).status.code ==
              ErrorCode::kUnavailable,
          "非 UTC 周期任务应明确报告展开能力未提供");

    return 0;
}
