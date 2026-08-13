#include "recurrence_planner.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "voicelife/schedule/calendar.h"

namespace voicelife::schedule {
namespace {

/// 东八区（UTC+8）时区偏移，无夏令时，MVP 固定。
constexpr int64_t kTimezoneOffsetSeconds = 8 * 3600;
constexpr int kDaysPerWeek = 7;

/// 东八区 civil time → UTC Unix 秒。
int64_t UnixFromLocal(int year, int month, int day, int hour, int minute, int second) {
    return DaysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second - kTimezoneOffsetSeconds;
}

/// UTC Unix 秒 → 东八区 civil time。
void LocalFromUnix(int64_t unix, int& year, int& month, int& day, int& hour, int& minute, int& second) {
    const int64_t local = unix + kTimezoneOffsetSeconds;
    CivilFromDays(local / 86400, year, month, day);
    const int64_t tod = local % 86400;
    hour = static_cast<int>(tod / 3600);
    minute = static_cast<int>((tod % 3600) / 60);
    second = static_cast<int>(tod % 60);
}

/// 正数向上取整除法（仅用于非负被除数）。
int64_t CeilDiv(int64_t dividend, int64_t divisor) { return (dividend + divisor - 1) / divisor; }

/// 比较两个本地日期，返回 -1/0/1。
int CompareDate(const LocalDate& left, const LocalDate& right) {
    if (left.year != right.year) return left.year < right.year ? -1 : 1;
    if (left.month != right.month) return left.month < right.month ? -1 : 1;
    if (left.day != right.day) return left.day < right.day ? -1 : 1;
    return 0;
}

/// 将本地日期 + 规则默认时刻转换为 UTC 秒。
DateTime OccurrenceAt(const ScheduleRule& rule, const LocalDate& date) {
    const int64_t unix = UnixFromLocal(date.year, date.month, date.day, rule.start_time.hour, rule.start_time.minute,
                                       rule.start_time.second);
    return DateTime{std::chrono::seconds{unix}};
}

/**
 * @brief 计算规则首次发生的本地日期（忽略 interval，即按 interval=1 找到第一个匹配日）。
 * @return 首个 ≥ start_date 的匹配日期；规则无效时为空。
 */
std::optional<LocalDate> FirstMatchingDate(const ScheduleRule& rule) {
    switch (rule.freq_type) {
        case Frequency::kDaily:
            return rule.start_date;  // 每天都是匹配日，首次 = start_date
        case Frequency::kWeekly: {
            if (!rule.weekdays_mask.has_value()) return std::nullopt;
            const int64_t start_days = DaysFromCivil(rule.start_date.year, rule.start_date.month, rule.start_date.day);
            const int start_weekday = Weekday(rule.start_date.year, rule.start_date.month, rule.start_date.day);
            for (int64_t week = 0; week < 200000; ++week) {
                const int64_t monday = start_days - start_weekday + week * kDaysPerWeek;
                for (int weekday = 0; weekday < kDaysPerWeek; ++weekday) {
                    if ((*rule.weekdays_mask & static_cast<uint8_t>(1u << weekday)) == 0) continue;
                    LocalDate date;
                    CivilFromDays(monday + weekday, date.year, date.month, date.day);
                    if (CompareDate(date, rule.start_date) >= 0) return date;
                }
            }
            return std::nullopt;
        }
        case Frequency::kMonthly: {
            const int64_t start_index = static_cast<int64_t>(rule.start_date.year) * 12 + (rule.start_date.month - 1);
            for (int64_t month = 0; month < 200000; ++month) {
                const int64_t month_index = start_index + month;
                const int year = static_cast<int>(month_index / 12);
                const int m = static_cast<int>(month_index % 12) + 1;
                int day;
                if (rule.monthly_mode == MonthlyMode::kLastDay) {
                    day = DaysInMonth(year, m);
                } else {
                    if (!rule.day_of_month.has_value()) return std::nullopt;
                    day = *rule.day_of_month;
                    if (day > DaysInMonth(year, m)) continue;  // 短月跳过
                }
                const LocalDate date{year, m, day};
                if (CompareDate(date, rule.start_date) >= 0) return date;
            }
            return std::nullopt;
        }
        case Frequency::kYearly: {
            if (!rule.month_of_year.has_value() || !rule.day_of_month.has_value()) return std::nullopt;
            for (int64_t year = 0; year < 200000; ++year) {
                const int y = rule.start_date.year + static_cast<int>(year);
                if (*rule.day_of_month > DaysInMonth(y, *rule.month_of_year)) continue;  // 2/29 非闰年跳过
                const LocalDate date{y, *rule.month_of_year, *rule.day_of_month};
                if (CompareDate(date, rule.start_date) >= 0) return date;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

/**
 * @brief 返回第 k 个周期单元（从首次发生锚定）内的候选日期。
 * @param rule 周期规则。
 * @param anchor 首次发生日期。
 * @param k 相对首次发生单元的偏移（0 = 首次发生所在单元）。
 */
std::vector<LocalDate> CandidateDates(const ScheduleRule& rule, const LocalDate& anchor, int64_t k) {
    switch (rule.freq_type) {
        case Frequency::kDaily: {
            const int64_t days = DaysFromCivil(anchor.year, anchor.month, anchor.day) + k * rule.interval_val;
            LocalDate date;
            CivilFromDays(days, date.year, date.month, date.day);
            return {date};
        }
        case Frequency::kWeekly: {
            if (!rule.weekdays_mask.has_value()) return {};
            const int64_t anchor_days = DaysFromCivil(anchor.year, anchor.month, anchor.day);
            const int anchor_weekday = Weekday(anchor.year, anchor.month, anchor.day);
            const int64_t week_monday = anchor_days - anchor_weekday + k * rule.interval_val * kDaysPerWeek;
            std::vector<LocalDate> dates;
            for (int weekday = 0; weekday < kDaysPerWeek; ++weekday) {
                if ((*rule.weekdays_mask & static_cast<uint8_t>(1u << weekday)) == 0) continue;
                LocalDate date;
                CivilFromDays(week_monday + weekday, date.year, date.month, date.day);
                dates.push_back(date);
            }
            return dates;
        }
        case Frequency::kMonthly: {
            const int64_t anchor_index = static_cast<int64_t>(anchor.year) * 12 + (anchor.month - 1);
            const int64_t month_index = anchor_index + k * rule.interval_val;
            const int year = static_cast<int>(month_index / 12);
            const int month = static_cast<int>(month_index % 12) + 1;
            int day;
            if (rule.monthly_mode == MonthlyMode::kLastDay) {
                day = DaysInMonth(year, month);
            } else {
                if (!rule.day_of_month.has_value()) return {};
                day = *rule.day_of_month;
                if (day > DaysInMonth(year, month)) return {};  // 短月跳过
            }
            return {LocalDate{year, month, day}};
        }
        case Frequency::kYearly: {
            if (!rule.month_of_year.has_value() || !rule.day_of_month.has_value()) return {};
            const int year = anchor.year + static_cast<int>(k * rule.interval_val);
            if (*rule.day_of_month > DaysInMonth(year, *rule.month_of_year)) return {};
            return {LocalDate{year, *rule.month_of_year, *rule.day_of_month}};
        }
    }
    return {};
}

/// 计算目标日期落在第几个周期单元（从首次发生锚定，用于跳过历史扫描）。
int64_t FirstUnitIndex(const ScheduleRule& rule, const LocalDate& anchor, const LocalDate& target) {
    const int64_t anchor_days = DaysFromCivil(anchor.year, anchor.month, anchor.day);
    const int64_t target_days = DaysFromCivil(target.year, target.month, target.day);
    switch (rule.freq_type) {
        case Frequency::kDaily:
            return std::max<int64_t>(0, CeilDiv(target_days - anchor_days, rule.interval_val));
        case Frequency::kWeekly: {
            const int anchor_weekday = Weekday(anchor.year, anchor.month, anchor.day);
            const int target_weekday = Weekday(target.year, target.month, target.day);
            const int64_t week_diff = ((target_days - target_weekday) - (anchor_days - anchor_weekday)) / kDaysPerWeek;
            return std::max<int64_t>(0, CeilDiv(week_diff, rule.interval_val));
        }
        case Frequency::kMonthly: {
            const int64_t anchor_index = static_cast<int64_t>(anchor.year) * 12 + (anchor.month - 1);
            const int64_t target_index = static_cast<int64_t>(target.year) * 12 + (target.month - 1);
            return std::max<int64_t>(0, CeilDiv(target_index - anchor_index, rule.interval_val));
        }
        case Frequency::kYearly:
            return std::max<int64_t>(0, CeilDiv(target.year - anchor.year, rule.interval_val));
    }
    return 0;
}

}  // namespace

std::optional<DateTime> NextOccurrence(const ScheduleRule& rule, DateTime from) {
    if (rule.status != ScheduleStatus::kActive) return std::nullopt;

    const std::optional<LocalDate> anchor = FirstMatchingDate(rule);
    if (!anchor.has_value()) return std::nullopt;

    int from_year = 0, from_month = 0, from_day = 0, from_hour = 0, from_minute = 0, from_second = 0;
    LocalFromUnix(from.time_since_epoch().count(), from_year, from_month, from_day, from_hour, from_minute, from_second);
    const LocalDate from_date{from_year, from_month, from_day};

    const int64_t k_start = FirstUnitIndex(rule, *anchor, from_date);
    // 安全上限：正常数年内即命中，上限仅用于防御异常规则。
    const int64_t k_limit = k_start + 200000;

    for (int64_t k = k_start; k < k_limit; ++k) {
        for (const LocalDate& date : CandidateDates(rule, *anchor, k)) {
            if (CompareDate(date, rule.start_date) < 0) continue;  // 首单元内早于生效日的匹配日
            if (rule.end_date.has_value() && CompareDate(date, *rule.end_date) > 0) return std::nullopt;
            const DateTime occurrence = OccurrenceAt(rule, date);
            if (occurrence < from) continue;
            return occurrence;
        }
    }
    return std::nullopt;
}

std::vector<DateTime> PlanOccurrences(const ScheduleRule& rule, DateTime range_start, DateTime range_end) {
    std::vector<DateTime> occurrences;
    DateTime cursor = range_start;
    for (int index = 0; index < 100000; ++index) {
        const std::optional<DateTime> next = NextOccurrence(rule, cursor);
        if (!next.has_value()) break;
        if (*next >= range_end) break;
        occurrences.push_back(*next);
        cursor = *next + std::chrono::seconds{1};
    }
    return occurrences;
}

}  // namespace voicelife::schedule
