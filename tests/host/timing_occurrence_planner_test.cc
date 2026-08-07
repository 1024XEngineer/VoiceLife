#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "support/test_support.h"
#include "voicelife/timing/occurrence_planner.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::timing::PlanOccurrences;
using voicelife::timing::RecurrenceFrequency;
using voicelife::timing::TimingTask;
using voicelife::timing::TimingTaskStatus;

namespace {

int64_t UtcAtLocal(int y, unsigned m, unsigned d, int hour, int minute = 0) {
    using namespace std::chrono;
    const sys_days local_day = year{y} / month{m} / day{d};
    const auto local_time = local_day + hours{hour} + minutes{minute};
    return duration_cast<seconds>((local_time - hours{8}).time_since_epoch()).count();
}

}  // namespace

int main() {
    constexpr int64_t kDay = 24 * 60 * 60;
    constexpr int64_t kLocalMonday = 1785688200;  // 2026-08-03 00:30 +08:00

    const TimingTask local_weekday_task{
        .id = "planner-local-weekday",
        .start_at = kLocalMonday,
        .time_zone = "+08:00",
        .recurrence = {.frequency = RecurrenceFrequency::kWeek, .by_weekdays = {1}},
        .status = TimingTaskStatus::kActive,
    };
    const auto local_weekday = PlanOccurrences(local_weekday_task, kLocalMonday, kLocalMonday + 7 * kDay);
    Check(local_weekday.ok() && local_weekday.value->size() == 1 && local_weekday.value->front() == kLocalMonday,
          "planner 应按 UTC+8 本地星期展开周期任务");

    TimingTask unsupported_task = local_weekday_task;
    unsupported_task.id = "planner-unsupported-zone";
    unsupported_task.time_zone = "UTC";
    const auto unsupported = PlanOccurrences(unsupported_task, kLocalMonday, kLocalMonday + kDay);
    Check(unsupported.status.code == ErrorCode::kUnavailable, "planner 应明确拒绝不支持的周期时区");

    const TimingTask one_time_task{
        .id = "planner-one-time",
        .start_at = kLocalMonday,
        .time_zone = "Asia/Shanghai",
        .status = TimingTaskStatus::kActive,
    };
    const auto one_time = PlanOccurrences(one_time_task, kLocalMonday, kLocalMonday + 1);
    Check(one_time.ok() && *one_time.value == std::vector<int64_t>{kLocalMonday}, "一次性任务应包含范围起点");
    const auto one_time_exclusive = PlanOccurrences(one_time_task, kLocalMonday + 1, kLocalMonday + 2);
    Check(one_time_exclusive.ok() && one_time_exclusive.value->empty(), "一次性任务应遵循左闭右开范围");

    const TimingTask daily_task{
        .id = "planner-daily",
        .start_at = kLocalMonday,
        .time_zone = "+08:00",
        .recurrence = {.frequency = RecurrenceFrequency::kDay},
        .status = TimingTaskStatus::kActive,
    };
    const auto daily = PlanOccurrences(daily_task, kLocalMonday, kLocalMonday + 3 * kDay);
    Check(
        daily.ok() && *daily.value == std::vector<int64_t>{kLocalMonday, kLocalMonday + kDay, kLocalMonday + 2 * kDay},
        "每日任务应按 UTC 时间升序展开");

    const auto weekly_default = PlanOccurrences(daily_task, kLocalMonday, kLocalMonday + 1);
    Check(weekly_default.ok() && weekly_default.value->size() == 1, "每日任务的单日窗口应稳定");
    TimingTask weekly_task = daily_task;
    weekly_task.id = "planner-weekly-default";
    weekly_task.recurrence = {.frequency = RecurrenceFrequency::kWeek};
    const auto weekly_default_anchor = PlanOccurrences(weekly_task, kLocalMonday, kLocalMonday + 15 * kDay);
    Check(weekly_default_anchor.ok() &&
              *weekly_default_anchor.value ==
                  std::vector<int64_t>{kLocalMonday, kLocalMonday + 7 * kDay, kLocalMonday + 14 * kDay},
          "每周任务未指定星期时应使用起始日期锚点");
    weekly_task.id = "planner-weekly-filter";
    weekly_task.recurrence.by_weekdays = {3};
    const auto weekly_filtered = PlanOccurrences(weekly_task, kLocalMonday, kLocalMonday + 15 * kDay);
    Check(weekly_filtered.ok() && weekly_filtered.value->size() == 2 &&
              weekly_filtered.value->front() == kLocalMonday + 2 * kDay,
          "每周任务应按本地星期筛选");

    const int64_t month_end = UtcAtLocal(2026, 8, 31, 0, 30);
    TimingTask monthly_task{
        .id = "planner-month-end",
        .start_at = month_end,
        .time_zone = "+08:00",
        .recurrence = {.frequency = RecurrenceFrequency::kMonth},
        .status = TimingTaskStatus::kActive,
    };
    const auto monthly = PlanOccurrences(monthly_task, month_end, month_end + 70 * kDay);
    Check(monthly.ok() && monthly.value->size() == 2 && monthly.value->at(1) == UtcAtLocal(2026, 10, 31, 0, 30),
          "每月任务应按本地月日展开并保留月末锚点语义");
    monthly_task.recurrence.by_month_days = {30};
    const auto monthly_filtered = PlanOccurrences(monthly_task, month_end, month_end + 70 * kDay);
    Check(monthly_filtered.ok() && *monthly_filtered.value == std::vector<int64_t>{UtcAtLocal(2026, 9, 30, 0, 30),
                                                                                   UtcAtLocal(2026, 10, 30, 0, 30)},
          "每月任务应按指定本地日期筛选");

    const int64_t leap_day = UtcAtLocal(2028, 2, 29, 9, 15);
    TimingTask yearly_task{
        .id = "planner-yearly-leap",
        .start_at = leap_day,
        .time_zone = "+08:00",
        .recurrence = {.frequency = RecurrenceFrequency::kYear},
        .status = TimingTaskStatus::kActive,
    };
    const auto yearly = PlanOccurrences(yearly_task, leap_day, UtcAtLocal(2033, 3, 1, 0));
    Check(yearly.ok() && *yearly.value == std::vector<int64_t>{leap_day, UtcAtLocal(2032, 2, 29, 9, 15)},
          "每年任务应正确处理闰年日期");
    yearly_task.recurrence = {.frequency = RecurrenceFrequency::kYear, .by_month_days = {5}, .by_months = {3, 4}};
    const auto yearly_filtered = PlanOccurrences(yearly_task, leap_day, UtcAtLocal(2029, 1, 1, 0));
    Check(yearly_filtered.ok() && *yearly_filtered.value == std::vector<int64_t>{UtcAtLocal(2028, 3, 5, 9, 15),
                                                                                 UtcAtLocal(2028, 4, 5, 9, 15)},
          "每年任务应按指定本地月份和日期筛选");

    TimingTask bounded_task = daily_task;
    bounded_task.id = "planner-effective-until";
    bounded_task.effective_until = kLocalMonday + 2 * kDay;
    const auto bounded = PlanOccurrences(bounded_task, kLocalMonday, kLocalMonday + 4 * kDay);
    Check(bounded.ok() && *bounded.value == std::vector<int64_t>{kLocalMonday, kLocalMonday + kDay},
          "effective_until 应作为首个不生成的时间点");
    const auto half_open = PlanOccurrences(daily_task, kLocalMonday + kDay, kLocalMonday + 2 * kDay);
    Check(half_open.ok() && *half_open.value == std::vector<int64_t>{kLocalMonday + kDay}, "规划范围结束点应排除");

    const auto overflow =
        PlanOccurrences(daily_task, std::numeric_limits<int64_t>::max() - 100, std::numeric_limits<int64_t>::max());
    Check(overflow.ok() && overflow.value->empty(), "UTC+8 偏移溢出时应返回空展开");
    TimingTask overflow_anchor = daily_task;
    overflow_anchor.start_at = std::numeric_limits<int64_t>::max();
    const auto overflow_start = PlanOccurrences(overflow_anchor, kLocalMonday, kLocalMonday + kDay);
    Check(overflow_start.ok() && overflow_start.value->empty(), "周期锚点的 UTC+8 偏移溢出时应返回空展开");
    Check(PlanOccurrences(daily_task, 10, 10).status.code == ErrorCode::kInvalidArgument, "空规划范围应返回参数错误");

    return 0;
}
