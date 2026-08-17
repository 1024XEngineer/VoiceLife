#include "recurrence_planner.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

#include "voicelife/schedule/calendar.h"

namespace voicelife::schedule {
namespace {

/// 东八区（UTC+8）时区偏移，无夏令时，MVP 固定。
constexpr int64_t kTimezoneOffsetSeconds = 8 * 3600;
constexpr int kDaysPerWeek = 7;
// 局部微扫只用于修正短月、闰日等边界，正常规则通常几步内就能命中。
constexpr int kMaxDateSearchSteps = 64;
// PlanOccurrences 显式传入的 limit 最终会收敛到这个上限，避免调用方误传大值。
constexpr int kMaxPlanLimit = 10;

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

LocalDate DateFromDays(int64_t days) {
    LocalDate date;
    CivilFromDays(days, date.year, date.month, date.day);
    return date;
}

/// 将本地日期按天数偏移；溢出时返回空，避免日期运算越界后产生错误结果。
std::optional<LocalDate> AddDaysChecked(const LocalDate& date, int64_t days) {
    const int64_t day_number = DaysFromCivil(date.year, date.month, date.day);
    if (days > 0 && day_number > std::numeric_limits<int64_t>::max() - days) return std::nullopt;
    if (days < 0 && day_number < std::numeric_limits<int64_t>::min() - days) return std::nullopt;
    return DateFromDays(day_number + days);
}

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

std::optional<LocalDate> NextDailyDate(const ScheduleRule& rule, const LocalDate& anchor, const LocalDate& target) {
    // 每日规则没有复杂过滤，直接按 anchor 到 target 的天数差向上取整到 interval 的整数倍。
    const int64_t anchor_days = DaysFromCivil(anchor.year, anchor.month, anchor.day);
    const int64_t target_days = DaysFromCivil(target.year, target.month, target.day);
    const int64_t days_after_anchor = target_days - anchor_days;
    const int64_t k = days_after_anchor <= 0 ? 0 : CeilDiv(days_after_anchor, rule.interval_val);
    return DateFromDays(anchor_days + k * rule.interval_val);
}

std::optional<LocalDate> NextWeeklyDate(const ScheduleRule& rule, const LocalDate& anchor, const LocalDate& target) {
    if (!rule.weekdays_mask.has_value()) return std::nullopt;

    // 统一用周一作为周起点，先把 anchor 和 target 都归一化到各自所在周的周一。
    const int64_t anchor_days = DaysFromCivil(anchor.year, anchor.month, anchor.day);
    const int anchor_weekday = Weekday(anchor.year, anchor.month, anchor.day);
    const int64_t anchor_week_monday = anchor_days - anchor_weekday;

    const int64_t target_days = DaysFromCivil(target.year, target.month, target.day);
    const int target_weekday = Weekday(target.year, target.month, target.day);
    const int64_t target_week_monday = target_days - target_weekday;
    const int64_t week_diff = target_days >= anchor_days ? (target_week_monday - anchor_week_monday) / kDaysPerWeek : 0;
    const int64_t k = target_days >= anchor_days ? std::max<int64_t>(0, CeilDiv(week_diff, rule.interval_val)) : 0;

    // 粗跳到目标周附近后，只需在连续几个周内找第一个命中星期，不需要长范围扫描。
    for (int attempt = 0; attempt < kMaxDateSearchSteps; ++attempt) {
        const int64_t week_monday =
            anchor_week_monday + (k + static_cast<int64_t>(attempt)) * rule.interval_val * kDaysPerWeek;
        for (int weekday = 0; weekday < kDaysPerWeek; ++weekday) {
            if ((*rule.weekdays_mask & static_cast<uint8_t>(1u << weekday)) == 0) continue;
            const LocalDate date = DateFromDays(week_monday + weekday);
            if (CompareDate(date, target) >= 0) return date;
        }
    }
    return std::nullopt;
}

std::optional<LocalDate> NextMonthlyDate(const ScheduleRule& rule, const LocalDate& anchor, const LocalDate& target) {
    // 将年月统一成绝对月序号，避免跨年时手动处理 12 -> 1 的边界。
    const int64_t anchor_index = static_cast<int64_t>(anchor.year) * 12 + (anchor.month - 1);
    const int64_t target_index = static_cast<int64_t>(target.year) * 12 + (target.month - 1);
    const int64_t months_after_anchor = target_index - anchor_index;
    const int64_t k = months_after_anchor <= 0 ? 0 : CeilDiv(months_after_anchor, rule.interval_val);

    // 短月可能没有 day_of_month，例如 31 号在 2 月不存在；跳过该月继续找下一个有效月。
    for (int attempt = 0; attempt < kMaxDateSearchSteps; ++attempt) {
        const int64_t month_index = anchor_index + (k + static_cast<int64_t>(attempt)) * rule.interval_val;
        const int year = static_cast<int>(month_index / 12);
        const int month = static_cast<int>(month_index % 12) + 1;
        int day;
        if (rule.monthly_mode == MonthlyMode::kLastDay) {
            day = DaysInMonth(year, month);
        } else {
            if (!rule.day_of_month.has_value()) return std::nullopt;
            day = *rule.day_of_month;
            if (day > DaysInMonth(year, month)) continue;  // 短月跳过，下一有效月仍可能匹配。
        }
        const LocalDate date{year, month, day};
        if (CompareDate(date, target) >= 0) return date;
    }
    return std::nullopt;
}

std::optional<LocalDate> NextYearlyDate(const ScheduleRule& rule, const LocalDate& anchor, const LocalDate& target) {
    if (!rule.month_of_year.has_value() || !rule.day_of_month.has_value()) return std::nullopt;

    // 年度规则只需比较年份，先粗跳到目标年份附近，再处理 2/29 这类非闰年跳过。
    const int64_t years_after_anchor = static_cast<int64_t>(target.year) - anchor.year;
    const int64_t k = years_after_anchor <= 0 ? 0 : CeilDiv(years_after_anchor, rule.interval_val);

    for (int attempt = 0; attempt < kMaxDateSearchSteps; ++attempt) {
        const int year = anchor.year + static_cast<int>((k + static_cast<int64_t>(attempt)) * rule.interval_val);
        if (*rule.day_of_month > DaysInMonth(year, *rule.month_of_year)) continue;  // 2/29 非闰年跳过。
        const LocalDate date{year, *rule.month_of_year, *rule.day_of_month};
        if (CompareDate(date, target) >= 0) return date;
    }
    return std::nullopt;
}

/// 从 anchor 所在周期单元开始，直接计算第一个 >= target 的候选日期。
std::optional<LocalDate> NextDateOnOrAfter(const ScheduleRule& rule, const LocalDate& anchor, const LocalDate& target) {
    switch (rule.freq_type) {
        case Frequency::kDaily:
            return NextDailyDate(rule, anchor, target);
        case Frequency::kWeekly:
            return NextWeeklyDate(rule, anchor, target);
        case Frequency::kMonthly:
            return NextMonthlyDate(rule, anchor, target);
        case Frequency::kYearly:
            return NextYearlyDate(rule, anchor, target);
    }
    return std::nullopt;
}

}  // namespace

LocalDate LocalDateFromUtc(DateTime time) {
    // 所有周期计算都以东八区本地日期为基准，时区换算在这里统一完成。
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    LocalFromUnix(time.time_since_epoch().count(), year, month, day, hour, minute, second);
    return {year, month, day};
}

std::optional<DateTime> NextOccurrence(const ScheduleRule& rule, DateTime from) {
    if (rule.status != ScheduleStatus::kActive) return std::nullopt;

    // rule.start_date 是规则的首个有效发生日，后续周期单元都从它开始推导。
    const LocalDate anchor = rule.start_date;
    int from_year = 0, from_month = 0, from_day = 0, from_hour = 0, from_minute = 0, from_second = 0;
    LocalFromUnix(from.time_since_epoch().count(), from_year, from_month, from_day, from_hour, from_minute,
                  from_second);
    const LocalDate from_date{from_year, from_month, from_day};

    LocalDate threshold = from_date;
    if (CompareDate(anchor, threshold) > 0) threshold = anchor;

    // 正常情况第一步就能算出候选日期；只有候选日时刻早于 from 时才向后推进一天再算一次。
    for (int attempt = 0; attempt < kMaxDateSearchSteps; ++attempt) {
        const std::optional<LocalDate> date = NextDateOnOrAfter(rule, anchor, threshold);
        if (!date.has_value()) return std::nullopt;
        if (CompareDate(*date, rule.start_date) < 0) {
            threshold = rule.start_date;
            continue;
        }
        if (rule.end_date.has_value() && CompareDate(*date, *rule.end_date) > 0) return std::nullopt;

        const DateTime occurrence = OccurrenceAt(rule, *date);
        if (occurrence < from) {
            // 候选日期满足规则，但当天 start_time 已经过去，因此从下一天继续查找。
            const std::optional<LocalDate> next_day = AddDaysChecked(*date, 1);
            if (!next_day.has_value()) return std::nullopt;
            threshold = *next_day;
            continue;
        }
        return occurrence;
    }
    return std::nullopt;
}

std::vector<DateTime> PlanOccurrences(const ScheduleRule& rule, DateTime range_start, DateTime range_end, int limit) {
    std::vector<DateTime> occurrences;
    // 默认 3 个，显式传入时最多也只返回 10 个；这是给嵌入式查询预留的硬上限。
    const int capped_limit = std::min(kMaxPlanLimit, std::max(0, limit));
    if (capped_limit == 0) return occurrences;

    DateTime cursor = range_start;
    for (int index = 0; index < capped_limit; ++index) {
        const std::optional<DateTime> next = NextOccurrence(rule, cursor);
        if (!next.has_value()) break;
        if (*next >= range_end) break;
        occurrences.push_back(*next);
        // 游标推进到命中点后 1 秒，保证下一次继续取“之后”的 occurrence。
        cursor = *next + std::chrono::seconds{1};
    }
    return occurrences;
}

}  // namespace voicelife::schedule
