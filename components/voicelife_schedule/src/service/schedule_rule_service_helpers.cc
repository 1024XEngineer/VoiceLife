#include "schedule_rule_service_helpers.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "../rules/recurrence_planner.h"
#include "../rules/schedule_time_rules.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_factory.h"

namespace voicelife::schedule::schedule_rule_service_helpers {
namespace {

constexpr std::size_t kMaximumEventLength = 100;
constexpr int kMaximumDayOfMonth = 31;
constexpr int kMaximumMonthOfYear = 12;
constexpr int64_t kTimezoneOffsetSeconds = 8 * 3600;

int CompareLocalDate(const LocalDate& left, const LocalDate& right) {
    if (left.year != right.year) return left.year < right.year ? -1 : 1;
    if (left.month != right.month) return left.month < right.month ? -1 : 1;
    if (left.day != right.day) return left.day < right.day ? -1 : 1;
    return 0;
}

}  // namespace

DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

DateTime AtLocalDate(const LocalDate& date, const LocalTime& time) {
    const int64_t days = DaysFromCivil(date.year, date.month, date.day);
    return DateTime{std::chrono::seconds{days * 86400 + LocalTimeToSeconds(time) - kTimezoneOffsetSeconds}};
}

Status ValidateRuleFields(const ScheduleRule& rule) {
    if (rule.event.empty()) return Status::Error(ErrorCode::kInvalidArgument, "规则名称不能为空");
    if (rule.event.length() > kMaximumEventLength)
        return Status::Error(ErrorCode::kInvalidArgument, "规则名称不能超过 100 个字符");
    if (rule.interval_val < 1) return Status::Error(ErrorCode::kInvalidArgument, "周期间隔必须大于零");
    if (rule.occurrence_count.has_value()) {
        return Status::Error(ErrorCode::kInvalidArgument, "当前版本暂不支持最大发生次数");
    }
    switch (rule.freq_type) {
        case Frequency::kWeekly:
            if (!rule.weekdays_mask.has_value() || *rule.weekdays_mask < 1 || *rule.weekdays_mask > 127) {
                return Status::Error(ErrorCode::kInvalidArgument, "每周规则必须提供有效的星期位图");
            }
            break;
        case Frequency::kMonthly:
            if (!rule.monthly_mode.has_value())
                return Status::Error(ErrorCode::kInvalidArgument, "每月规则必须提供月模式");
            if (*rule.monthly_mode == MonthlyMode::kSpecificDay && !rule.day_of_month.has_value()) {
                return Status::Error(ErrorCode::kInvalidArgument, "指定日期模式必须提供日期");
            }
            if (*rule.monthly_mode == MonthlyMode::kSpecificDay &&
                (*rule.day_of_month < 1 || *rule.day_of_month > kMaximumDayOfMonth)) {
                return Status::Error(ErrorCode::kInvalidArgument, "每月指定日期必须在 1 到 31 之间");
            }
            break;
        case Frequency::kYearly:
            if (!rule.month_of_year.has_value() || !rule.day_of_month.has_value()) {
                return Status::Error(ErrorCode::kInvalidArgument, "每年规则必须提供月份和日期");
            }
            if (*rule.month_of_year < 1 || *rule.month_of_year > kMaximumMonthOfYear) {
                return Status::Error(ErrorCode::kInvalidArgument, "每年规则月份必须在 1 到 12 之间");
            }
            if (*rule.day_of_month < 1 || *rule.day_of_month > kMaximumDayOfMonth) {
                return Status::Error(ErrorCode::kInvalidArgument, "每年规则日期必须在 1 到 31 之间");
            }
            if (*rule.day_of_month > DaysInMonth(2000, *rule.month_of_year)) {
                return Status::Error(ErrorCode::kInvalidArgument, "每年规则月份与日期组合必须有效");
            }
            break;
        case Frequency::kDaily:
            break;
    }
    if (rule.start_time.hour < 0 || rule.start_time.hour > 23 || rule.start_time.minute < 0 ||
        rule.start_time.minute > 59 || rule.start_time.second < 0 || rule.start_time.second > 59) {
        return Status::Error(ErrorCode::kInvalidArgument, "规则开始时间必须在有效时钟范围内");
    }
    if (rule.end_time.has_value() && LocalTimeToSeconds(*rule.end_time) <= LocalTimeToSeconds(rule.start_time)) {
        return Status::Error(ErrorCode::kInvalidArgument, "规则结束时间必须晚于开始时间");
    }
    if (rule.end_time.has_value() &&
        (rule.end_time->hour < 0 || rule.end_time->hour > 23 || rule.end_time->minute < 0 ||
         rule.end_time->minute > 59 || rule.end_time->second < 0 || rule.end_time->second > 59)) {
        return Status::Error(ErrorCode::kInvalidArgument, "规则结束时间必须在有效时钟范围内");
    }
    return Status::Ok();
}

Status ValidateRuleDateRange(const ScheduleRule& rule) {
    if (rule.end_date.has_value() && CompareLocalDate(*rule.end_date, rule.start_date) < 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "规则失效日期不能早于生效日期");
    }
    return Status::Ok();
}

std::vector<DateTime> NextOccurrences(const ScheduleRule& rule, DateTime from, int n) {
    std::vector<DateTime> result;
    DateTime cursor = from;
    for (int index = 0; index < n; ++index) {
        const std::optional<DateTime> next = NextOccurrence(rule, cursor);
        if (!next.has_value()) break;
        result.push_back(*next);
        cursor = *next + std::chrono::seconds{1};
    }
    return result;
}

bool MatchesKeyword(const ScheduleRule& rule, const std::string& keyword) {
    if (keyword.empty()) return true;
    if (rule.event.find(keyword) != std::string::npos) return true;
    if (rule.location.has_value() && rule.location->find(keyword) != std::string::npos) return true;
    if (rule.notes.has_value() && rule.notes->find(keyword) != std::string::npos) return true;
    return false;
}

bool MatchesStatus(const ScheduleRule& rule, ScheduleStatusFilter filter) {
    switch (filter) {
        case ScheduleStatusFilter::kAll:
            return true;
        case ScheduleStatusFilter::kActive:
            return rule.status == ScheduleStatus::kActive;
        case ScheduleStatusFilter::kCancelled:
            return rule.status == ScheduleStatus::kCancelled;
        case ScheduleStatusFilter::kCompleted:
            return rule.status == ScheduleStatus::kCompleted;
    }
    return false;
}

}  // namespace voicelife::schedule::schedule_rule_service_helpers
