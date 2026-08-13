#include "voicelife/schedule/calendar.h"

namespace voicelife::schedule {

// 基础公历工具，供周期规则在本地日期和 Unix 天数之间转换。
bool IsLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

int DaysInMonth(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year)) return 29;
    return kDays[month - 1];
}

// Howard Hinnant 风格的 civil date 换算，用于跳过日期表并保持统一 UTC 偏移。
std::int64_t DaysFromCivil(int year, int month, int day) {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// 将 Unix 天数还原为公历年月日，供周期规则按东八区本地日期计算。
void CivilFromDays(std::int64_t days, int& year, int& month, int& day) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    year += (month <= 2);
}

// 返回 ISO 风格星期编号（0 = 周一），周期周规则用它匹配 weekdays_mask。
int Weekday(int year, int month, int day) {
    const std::int64_t days = DaysFromCivil(year, month, day);
    const int weekday = static_cast<int>((days + 3) % 7);
    return weekday < 0 ? weekday + 7 : weekday;
}

}  // namespace voicelife::schedule
