#include "voicelife/schedule/calendar.h"

namespace voicelife::schedule {

// 基础公历工具，供周期规则在本地日期和 Unix 天数之间转换。
bool IsLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

int DaysInMonth(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year)) return 29;
    return kDays[month - 1];
}

/**
 * @brief 将公历年月日换算成相对 1970-01-01 的天数。
 *
 * 这里不是从 1970-01-01 开始逐日累加，而是按公历的 400 年周期、年内月份偏移
 * 直接数学换算，避免日期越远计算量越大。
 */
std::int64_t DaysFromCivil(int year, int month, int day) {
    // 把 1、2 月并入上一年，让 3 月到次年 2 月成为一个连续的“年周期”，便于统一处理闰年。
    year -= month <= 2;
    // 公历每 400 年循环一次，era 是第几个 400 年周期。
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    // yoe 是当前 400 年周期内的第几年。
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    // doy 是调整后“年周期”内的第几天；月份累计天数由公式直接算出，不逐月扫描。
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    // doe 是当前 400 年周期内已经经过的天数，包含普通年天数和闰年补偿。
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    // 146097 是一个 400 年周期的总天数，719468 是相对 1970-01-01 的基准偏移。
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

/**
 * @brief 根据年月日计算星期几。
 *
 * 先得到相对 1970-01-01 的总天数，再对 7 取余得到星期编号。1970-01-01 是周四，
 * 因此需要 +3 做偏移，使返回值固定为 0=周一，6=周日。
 */
int Weekday(int year, int month, int day) {
    const std::int64_t days = DaysFromCivil(year, month, day);
    const int weekday = static_cast<int>((days + 3) % 7);
    return weekday < 0 ? weekday + 7 : weekday;
}

}  // namespace voicelife::schedule
