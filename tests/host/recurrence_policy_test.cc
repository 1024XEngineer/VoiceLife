#include "support/test_support.h"
#include "voicelife/timing/recurrence_policy.h"

using voicelife::test::Check;

int main() {
    using namespace voicelife::timing;
    RecurrencePolicy policy;
    const int64_t monday = 1785686400;  // 2026-08-03 00:00:00 UTC.

    const auto weekly = policy.Expand({.frequency = RecurrenceFrequency::kWeek, .start_at = monday,
                                       .time_zone = "Asia/Shanghai", .by_weekdays = {1, 3}},
                                      monday, monday + 8 * 86400);
    Check(weekly.ok() && weekly.value->size() == 3, "每周规则应展开周一、周三和下一周周一");

    const auto monthly = policy.Expand({.frequency = RecurrenceFrequency::kMonth, .start_at = monday,
                                        .time_zone = "Asia/Shanghai", .by_month_days = {3}},
                                       monday, monday + 40 * 86400);
    Check(monthly.ok() && monthly.value->size() == 2, "每月规则应跨月展开指定日期");

    const auto yearly = policy.NextAfter({.frequency = RecurrenceFrequency::kYear, .start_at = monday,
                                           .time_zone = "Asia/Shanghai", .by_month_days = {3},
                                           .by_months = {8}}, monday);
    Check(yearly.ok() && *yearly.value > monday + 360 * 86400LL, "每年规则应推进到下一年");
    const int64_t leap_day_2024 = 1709164800;
    const auto next_leap_day = policy.NextAfter({
        .frequency = RecurrenceFrequency::kYear, .start_at = leap_day_2024,
        .time_zone = "UTC", .by_month_days = {29}, .by_months = {2},
    }, leap_day_2024);
    Check(next_leap_day.ok() && *next_leap_day.value == 1835395200,
          "年度规则应能跨越非闰年找到下一次 2 月 29 日");
    Check(policy.Validate({.frequency = RecurrenceFrequency::kDay, .start_at = monday,
                           .time_zone = "+08:00"}).code == voicelife::ErrorCode::kInvalidArgument,
          "周期规则必须使用受支持的 IANA 时区");
    return 0;
}
