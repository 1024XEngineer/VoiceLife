#pragma once

#include <cstdint>

namespace voicelife::schedule {

/** @brief 判断闰年。 @param year 年份。 @return 闰年返回 true。 */
bool IsLeapYear(int year);

/**
 * @brief 返回某月天数。
 * @param year 年份。
 * @param month 月份（1~12）。
 * @return 该月天数。
 */
int DaysInMonth(int year, int month);

/**
 * @brief 自 1970-01-01 起的天数（Howard Hinnant civil 算法）。
 * @param year 年。 @param month 月。 @param day 日。
 * @return 自纪元起的天数。
 */
std::int64_t DaysFromCivil(int year, int month, int day);

/**
 * @brief 自 1970-01-01 起的天数反解为年月日。
 * @param days 自纪元起的天数。
 * @param year 输出年。 @param month 输出月。 @param day 输出日。
 */
void CivilFromDays(std::int64_t days, int& year, int& month, int& day);

/**
 * @brief 返回星期几。
 * @param year 年。 @param month 月。 @param day 日。
 * @return 0=周一 … 6=周日。
 */
int Weekday(int year, int month, int day);

}  // namespace voicelife::schedule
