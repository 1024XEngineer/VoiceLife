#include "schedule_rule_service_helpers.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "support/test_support.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_factory.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::Frequency;
using voicelife::schedule::LocalDate;
using voicelife::schedule::LocalTime;
using voicelife::schedule::MonthlyMode;
using voicelife::schedule::ScheduleRule;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ScheduleStatusFilter;
using voicelife::schedule::schedule_rule_service_helpers::AtLocalDate;
using voicelife::schedule::schedule_rule_service_helpers::MatchesKeyword;
using voicelife::schedule::schedule_rule_service_helpers::MatchesStatus;
using voicelife::schedule::schedule_rule_service_helpers::NextOccurrences;
using voicelife::schedule::schedule_rule_service_helpers::ValidateRuleDateRange;
using voicelife::schedule::schedule_rule_service_helpers::ValidateRuleFields;
using voicelife::test::Check;

namespace {

/** @brief 转换 Unix 秒。 @param seconds Unix 秒。 @return 日程时间。 */
DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/** @brief 构造测试默认每日规则。 @return 每日 09:00 规则。 */
ScheduleRule DailyRule() {
    ScheduleRule rule;
    rule.event = "每日例会";
    rule.freq_type = Frequency::kDaily;
    rule.interval_val = 1;
    rule.start_time = LocalTime{9, 0, 0};
    rule.start_date = LocalDate{2099, 1, 1};
    rule.status = ScheduleStatus::kActive;
    return rule;
}

}  // namespace

int main() {
    const DateTime local = AtLocalDate(LocalDate{2099, 1, 1}, LocalTime{9, 30, 0});
    const int64_t expected = voicelife::schedule::DaysFromCivil(2099, 1, 1) * 86400 + 9 * 3600 + 30 * 60 - 8 * 3600;
    Check(local.time_since_epoch().count() == expected, "AtLocalDate 必须按东八区本地时间换算");

    const ScheduleRule daily = DailyRule();
    const auto occurrences = NextOccurrences(daily, At(expected), 3);
    Check(occurrences.size() == 3, "NextOccurrences 应返回指定数量的未来发生时间");

    auto cancelled = DailyRule();
    cancelled.status = ScheduleStatus::kCancelled;
    Check(!MatchesStatus(cancelled, ScheduleStatusFilter::kActive) &&
              MatchesStatus(cancelled, ScheduleStatusFilter::kCancelled),
          "MatchesStatus 应区分取消和活跃状态");
    Check(MatchesStatus(daily, ScheduleStatusFilter::kAll) && MatchesStatus(daily, ScheduleStatusFilter::kActive),
          "MatchesStatus 应命中全部和活跃筛选");

    Check(MatchesKeyword(daily, "") && MatchesKeyword(daily, "例会"), "空关键词和命中事件应通过");
    ScheduleRule located = DailyRule();
    located.location = "会议室";
    located.notes = "复盘";
    Check(MatchesKeyword(located, "会议") && MatchesKeyword(located, "复盘") && !MatchesKeyword(located, "不存在的词"),
          "关键词应匹配地点和备注");

    Check(ValidateRuleFields(DailyRule()).ok(), "合法每日规则应通过校验");
    ScheduleRule empty_event = DailyRule();
    empty_event.event.clear();
    Check(ValidateRuleFields(empty_event).code == ErrorCode::kInvalidArgument, "空规则名称应校验失败");
    ScheduleRule invalid_interval = DailyRule();
    invalid_interval.interval_val = 0;
    Check(ValidateRuleFields(invalid_interval).code == ErrorCode::kInvalidArgument, "无效间隔应校验失败");
    ScheduleRule count_unsupported = DailyRule();
    count_unsupported.occurrence_count = 5;
    Check(ValidateRuleFields(count_unsupported).code == ErrorCode::kInvalidArgument, "当前版本应拒绝最大次数");

    ScheduleRule weekly = DailyRule();
    weekly.freq_type = Frequency::kWeekly;
    Check(ValidateRuleFields(weekly).code == ErrorCode::kInvalidArgument, "每周规则缺少星期位图应失败");
    weekly.weekdays_mask = 1;
    Check(ValidateRuleFields(weekly).ok(), "每周规则提供星期位图后应通过");

    ScheduleRule monthly = DailyRule();
    monthly.freq_type = Frequency::kMonthly;
    monthly.monthly_mode = MonthlyMode::kLastDay;
    Check(ValidateRuleFields(monthly).ok(), "每月最后一天模式应通过");
    monthly.monthly_mode = MonthlyMode::kSpecificDay;
    monthly.day_of_month = 31;
    Check(ValidateRuleFields(monthly).ok(), "每月指定日期模式应通过");
    monthly.day_of_month = 0;
    Check(ValidateRuleFields(monthly).code == ErrorCode::kInvalidArgument, "每月指定日期越界应失败");

    ScheduleRule yearly = DailyRule();
    yearly.freq_type = Frequency::kYearly;
    Check(ValidateRuleFields(yearly).code == ErrorCode::kInvalidArgument, "每年规则缺少日期应失败");
    yearly.month_of_year = 2;
    yearly.day_of_month = 29;
    Check(ValidateRuleFields(yearly).ok(), "每年规则 2 月 29 日应通过基准校验");
    yearly.day_of_month = 30;
    Check(ValidateRuleFields(yearly).code == ErrorCode::kInvalidArgument, "每年规则无效日期应失败");

    ScheduleRule invalid_time = DailyRule();
    invalid_time.start_time = LocalTime{24, 0, 0};
    Check(ValidateRuleFields(invalid_time).code == ErrorCode::kInvalidArgument, "规则开始时间越界应失败");
    ScheduleRule invalid_end = DailyRule();
    invalid_end.end_time = LocalTime{8, 0, 0};
    Check(ValidateRuleFields(invalid_end).code == ErrorCode::kInvalidArgument, "结束时间早于开始时间应失败");

    ScheduleRule date_ok = DailyRule();
    date_ok.end_date = LocalDate{2099, 1, 31};
    Check(ValidateRuleDateRange(date_ok).ok(), "失效日期不早于开始日期应通过");
    date_ok.end_date = LocalDate{2098, 12, 31};
    Check(ValidateRuleDateRange(date_ok).code == ErrorCode::kInvalidArgument, "失效日期早于开始日期应失败");
    return 0;
}
