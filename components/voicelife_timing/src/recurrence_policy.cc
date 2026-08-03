#include "voicelife/timing/recurrence_policy.h"

#include <algorithm>
#include <tuple>

namespace voicelife::timing {
namespace {

int64_t ZoneOffset(const std::string& zone) {
    if (zone == "UTC") return 0;
    if (zone == "Asia/Shanghai") return 8 * 3600;
    return INT64_MIN;
}

std::tuple<int, unsigned, unsigned> CivilFromDays(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    return {y, m, d};
}

int Weekday(int64_t local_day) {
    const int value = static_cast<int>((local_day + 3) % 7);
    return (value < 0 ? value + 7 : value) + 1;
}

bool Contains(const std::vector<int>& values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool Matches(const RecurrenceRule& rule, int64_t day, int start_weekday, int start_month_day, int start_month) {
    const auto [year, month, month_day] = CivilFromDays(day);
    (void)year;
    switch (rule.frequency) {
        case RecurrenceFrequency::kNone: return day == (rule.start_at + ZoneOffset(rule.time_zone)) / 86400;
        case RecurrenceFrequency::kDay: return true;
        case RecurrenceFrequency::kWeek:
            return Contains(rule.by_weekdays.empty() ? std::vector<int>{start_weekday} : rule.by_weekdays,
                            Weekday(day));
        case RecurrenceFrequency::kMonth:
            return Contains(rule.by_month_days.empty() ? std::vector<int>{start_month_day} : rule.by_month_days,
                            static_cast<int>(month_day));
        case RecurrenceFrequency::kYear:
            return Contains(rule.by_months.empty() ? std::vector<int>{start_month} : rule.by_months,
                            static_cast<int>(month)) &&
                   Contains(rule.by_month_days.empty() ? std::vector<int>{start_month_day} : rule.by_month_days,
                            static_cast<int>(month_day));
    }
    return false;
}

}  // namespace

Status RecurrencePolicy::Validate(const RecurrenceRule& rule) const {
    if (rule.start_at <= 0 || ZoneOffset(rule.time_zone) == INT64_MIN) {
        return Status::Error(ErrorCode::kInvalidArgument, "周期规则需要有效开始时间和受支持的 IANA 时区");
    }
    for (int value : rule.by_weekdays) if (value < 1 || value > 7) return Status::Error(ErrorCode::kInvalidArgument, "星期必须在 1 到 7 之间");
    for (int value : rule.by_month_days) if (value < 1 || value > 31) return Status::Error(ErrorCode::kInvalidArgument, "日期必须在 1 到 31 之间");
    for (int value : rule.by_months) if (value < 1 || value > 12) return Status::Error(ErrorCode::kInvalidArgument, "月份必须在 1 到 12 之间");
    return Status::Ok();
}

Result<std::vector<int64_t>> RecurrencePolicy::Expand(const RecurrenceRule& rule, int64_t begin, int64_t end) const {
    const Status valid = Validate(rule);
    if (!valid.ok()) return Result<std::vector<int64_t>>::Failure(valid.code, valid.message);
    if (end <= begin) return Result<std::vector<int64_t>>::Failure(ErrorCode::kInvalidArgument, "展开范围无效");
    const int64_t offset = ZoneOffset(rule.time_zone);
    const int64_t start_local = rule.start_at + offset;
    const int64_t start_day = start_local / 86400;
    const int64_t seconds = start_local % 86400;
    const auto [year, month, month_day] = CivilFromDays(start_day);
    (void)year;
    std::vector<int64_t> out;
    int64_t day = std::max(start_day, (begin + offset) / 86400 - 1);
    const int64_t last = (end + offset) / 86400 + 1;
    for (; day <= last; ++day) {
        if (!Matches(rule, day, Weekday(start_day), static_cast<int>(month_day), static_cast<int>(month))) continue;
        const int64_t occurrence = day * 86400 + seconds - offset;
        if (occurrence >= rule.start_at && occurrence >= begin && occurrence < end) out.push_back(occurrence);
    }
    return Result<std::vector<int64_t>>::Success(std::move(out));
}

Result<int64_t> RecurrencePolicy::NextAfter(const RecurrenceRule& rule, int64_t occurrence_at) const {
    if (rule.frequency == RecurrenceFrequency::kNone) return Result<int64_t>::Success(0);
    const int64_t search_days = rule.frequency == RecurrenceFrequency::kYear ? 8 * 366LL : 370LL;
    auto expanded = Expand(rule, occurrence_at + 1, occurrence_at + search_days * 86400LL);
    if (!expanded.ok()) return Result<int64_t>::Failure(expanded.status.code, expanded.status.message);
    if (expanded.value->empty()) return Result<int64_t>::Failure(ErrorCode::kInternal, "无法计算下一次 occurrence");
    return Result<int64_t>::Success(expanded.value->front());
}

}  // namespace voicelife::timing
