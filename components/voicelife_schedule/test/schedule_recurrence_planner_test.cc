#include <chrono>
#include <cstdint>
#include <optional>

#include "rules/recurrence_planner.h"
#include "support/test_support.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_types.h"

using voicelife::schedule::DateTime;
using voicelife::schedule::Frequency;
using voicelife::schedule::LocalTime;
using voicelife::schedule::MonthlyMode;
using voicelife::schedule::NextOccurrence;
using voicelife::schedule::PlanOccurrences;
using voicelife::schedule::ScheduleRule;
using voicelife::schedule::ScheduleStatus;
using voicelife::test::Check;

namespace {

DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/// 按东八区本地时间构造 Unix 秒，方便测试直接表达业务日期和时刻。
int64_t UtcAtLocal(int year, int month, int day, int hour, int minute = 0, int second = 0) {
    using namespace voicelife::schedule;
    return DaysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second - 8 * 3600;
}

ScheduleRule BaseRule() {
    // 默认规则固定为每日 09:00，测试中再按频率覆盖字段。
    ScheduleRule rule;
    rule.event = "周期规则";
    rule.freq_type = Frequency::kDaily;
    rule.interval_val = 1;
    rule.start_time = LocalTime{9, 0, 0};
    rule.start_date = {2026, 8, 1};
    rule.status = ScheduleStatus::kActive;
    return rule;
}

}  // namespace

int main() {
    // yearly 验证粗跳后直接定位到目标年的 9 月 1 日，而不是逐日扫描。
    ScheduleRule yearly = BaseRule();
    yearly.freq_type = Frequency::kYearly;
    yearly.month_of_year = 9;
    yearly.day_of_month = 1;
    yearly.start_date = {2025, 9, 1};
    const std::optional<DateTime> yearly_next = NextOccurrence(yearly, At(UtcAtLocal(2026, 8, 14, 12)));
    Check(yearly_next.has_value() && yearly_next->time_since_epoch().count() == UtcAtLocal(2026, 9, 1, 9),
          "每年规则应直接计算当前年之后的 9 月 1 日");

    ScheduleRule daily = BaseRule();
    const std::optional<DateTime> daily_next = NextOccurrence(daily, At(UtcAtLocal(2026, 8, 1, 10)));
    Check(daily_next.has_value() && daily_next->time_since_epoch().count() == UtcAtLocal(2026, 8, 2, 9),
          "每日规则在同日时刻已过后应直接跳到下一日");

    ScheduleRule monthly = BaseRule();
    monthly.freq_type = Frequency::kMonthly;
    monthly.monthly_mode = MonthlyMode::kLastDay;
    monthly.start_date = {2026, 8, 31};
    const std::optional<DateTime> monthly_next = NextOccurrence(monthly, At(UtcAtLocal(2026, 9, 1, 0)));
    Check(monthly_next.has_value() && monthly_next->time_since_epoch().count() == UtcAtLocal(2026, 9, 30, 9),
          "每月最后一天规则应计算到下个有效月末");

    const auto planned = PlanOccurrences(daily, At(UtcAtLocal(2026, 8, 1, 9)), At(UtcAtLocal(2026, 8, 4, 0)));
    Check(planned.size() == 3, "PlanOccurrences 默认最多返回 3 个 occurrence");

    const auto limited = PlanOccurrences(daily, At(UtcAtLocal(2026, 8, 1, 9)), At(UtcAtLocal(2026, 8, 20, 0)), 2);
    Check(limited.size() == 2, "PlanOccurrences 应按显式参数限制返回数量");

    const auto capped = PlanOccurrences(daily, At(UtcAtLocal(2026, 8, 1, 9)), At(UtcAtLocal(2026, 8, 20, 0)), 10000);
    Check(capped.size() == 10, "PlanOccurrences 显式数量超过上限时应收敛到 10");

    return 0;
}
