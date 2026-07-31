#include "voicelife_domain.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <esp_random.h>

namespace voicelife {
namespace {

constexpr int64_t kShanghaiOffsetSeconds = 8 * 60 * 60;

struct ParsedIso8601 {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int offset_minutes = 0;
    int64_t epoch_seconds = -1;
};

int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool ValidDate(int year, int month, int day) {
    if (year < 1970 || month < 1 || month > 12 || day < 1) return false;
    static constexpr int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit = days_in_month[month - 1];
    if (month == 2 && IsLeapYear(year)) limit = 29;
    return day <= limit;
}

int DaysInMonth(int year, int month) {
    static constexpr int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && IsLeapYear(year)) return 29;
    return days_in_month[month - 1];
}

bool ParseInt(const char* text, int length, int* value) {
    if (text == nullptr || value == nullptr || length <= 0) return false;
    int parsed = 0;
    for (int i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        parsed = parsed * 10 + (text[i] - '0');
    }
    *value = parsed;
    return true;
}

bool ParseIso8601Parts(const std::string& value, ParsedIso8601* parsed) {
    if (parsed == nullptr || value.size() < 16) return false;
    ParsedIso8601 result;
    if (!ParseInt(value.data(), 4, &result.year) || value[4] != '-' ||
        !ParseInt(value.data() + 5, 2, &result.month) || value[7] != '-' ||
        !ParseInt(value.data() + 8, 2, &result.day) ||
        (value[10] != 'T' && value[10] != 't' && value[10] != ' ') ||
        !ParseInt(value.data() + 11, 2, &result.hour) || value[13] != ':' ||
        !ParseInt(value.data() + 14, 2, &result.minute)) {
        return false;
    }
    size_t cursor = 16;
    if (cursor < value.size() && value[cursor] == ':') {
        if (cursor + 3 > value.size() || !ParseInt(value.data() + cursor + 1, 2, &result.second)) return false;
        cursor += 3;
    }
    if (cursor < value.size() && value[cursor] == '.') {
        const size_t fraction_start = ++cursor;
        while (cursor < value.size() && std::isdigit(static_cast<unsigned char>(value[cursor]))) ++cursor;
        if (cursor == fraction_start) return false;
    }
    if (!ValidDate(result.year, result.month, result.day) ||
        result.hour > 23 || result.minute > 59 || result.second > 59) {
        return false;
    }

    if (cursor < value.size() && (value[cursor] == 'Z' || value[cursor] == 'z')) {
        ++cursor;
    } else if (cursor < value.size() && (value[cursor] == '+' || value[cursor] == '-')) {
        const int sign = value[cursor] == '+' ? 1 : -1;
        ++cursor;
        int offset_hour = 0;
        int offset_minute = 0;
        if (cursor + 2 > value.size() || !ParseInt(value.data() + cursor, 2, &offset_hour)) return false;
        cursor += 2;
        if (cursor < value.size() && value[cursor] == ':') ++cursor;
        if (cursor + 2 > value.size() || !ParseInt(value.data() + cursor, 2, &offset_minute)) return false;
        cursor += 2;
        if (offset_hour > 23 || offset_minute > 59) return false;
        result.offset_minutes = sign * (offset_hour * 60 + offset_minute);
    } else {
        return false;
    }
    if (cursor != value.size()) return false;

    const int64_t local_seconds =
        DaysFromCivil(result.year, static_cast<unsigned>(result.month), static_cast<unsigned>(result.day)) * 86400LL +
        result.hour * 3600LL + result.minute * 60LL + result.second;
    result.epoch_seconds = local_seconds - result.offset_minutes * 60LL;
    *parsed = result;
    return true;
}

bool ShanghaiTime(int64_t epoch_seconds, std::tm* value) {
    if (epoch_seconds < 0 || value == nullptr) return false;
    const std::time_t local = static_cast<std::time_t>(epoch_seconds + kShanghaiOffsetSeconds);
    return gmtime_r(&local, value) != nullptr;
}

int64_t ShanghaiDay(int64_t epoch_seconds) {
    return (epoch_seconds + kShanghaiOffsetSeconds) / 86400LL;
}

int IsoWeekdayFromCivilDay(int64_t days) {
    int weekday = static_cast<int>((days + 3) % 7);
    if (weekday < 0) weekday += 7;
    return weekday + 1;
}

std::string FormatAtOffset(int64_t epoch_seconds, int offset_minutes) {
    const std::time_t local =
        static_cast<std::time_t>(epoch_seconds + offset_minutes * 60LL);
    std::tm value{};
    if (gmtime_r(&local, &value) == nullptr) return {};

    const char sign = offset_minutes < 0 ? '-' : '+';
    const int absolute_offset = std::abs(offset_minutes);
    char buffer[40]{};
    const int written = std::snprintf(
        buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
        value.tm_year + 1900, value.tm_mon + 1, value.tm_mday, value.tm_hour, value.tm_min,
        value.tm_sec, sign, absolute_offset / 60, absolute_offset % 60);
    return written > 0 && static_cast<size_t>(written) < sizeof(buffer) ? std::string(buffer)
                                                                       : std::string{};
}

const char* SpokenPeriod(int hour) {
    if (hour < 6) return "凌晨";
    if (hour < 12) return "上午";
    if (hour == 12) return "中午";
    if (hour < 18) return "下午";
    return "晚上";
}

std::string SpokenClock(const std::tm& value) {
    int hour = value.tm_hour % 12;
    if (hour == 0) hour = 12;
    std::string text = std::to_string(hour) + "点";
    if (value.tm_min == 30) return text + "半";
    if (value.tm_min != 0) text += std::to_string(value.tm_min) + "分";
    return text;
}

std::string SpokenDatePrefix(const std::tm& target, const std::tm& now, int64_t day_delta) {
    const std::string period = SpokenPeriod(target.tm_hour);
    if (day_delta == 0) return period == "晚上" ? "今晚" : "今天" + period;
    if (day_delta == 1) return "明天" + period;
    if (day_delta == -1) return "昨天" + period;

    std::string prefix;
    if (target.tm_year != now.tm_year) {
        prefix += std::to_string(target.tm_year + 1900) + "年";
    }
    prefix += std::to_string(target.tm_mon + 1) + "月" +
              std::to_string(target.tm_mday) + "日";
    return prefix + period;
}

}  // namespace

int64_t ParseIso8601(const std::string& value) {
    // The service deliberately accepts the forms emitted by the #62 client:
    // YYYY-MM-DDTHH:MM[:SS][.fraction](Z|+HH:MM|-HH:MM).
    ParsedIso8601 parsed;
    return ParseIso8601Parts(value, &parsed) ? parsed.epoch_seconds : -1;
}

std::string FormatUtc(int64_t epoch_seconds) {
    std::time_t raw = static_cast<std::time_t>(epoch_seconds);
    std::tm tm_value{};
    if (gmtime_r(&raw, &tm_value) == nullptr) return {};
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_value) == 0) return {};
    return buffer;
}

std::string FormatSpokenTime(const std::string& value, int64_t now_epoch_seconds) {
    const int64_t target_epoch = ParseIso8601(value);
    std::tm target{};
    std::tm now{};
    if (!ShanghaiTime(target_epoch, &target) || !ShanghaiTime(now_epoch_seconds, &now)) {
        return "时间待确认";
    }
    const int64_t day_delta = ShanghaiDay(target_epoch) - ShanghaiDay(now_epoch_seconds);
    return SpokenDatePrefix(target, now, day_delta) + SpokenClock(target);
}

std::string FormatSpokenTimeRange(const std::string& starts_at, const std::string& ends_at,
                                  int64_t now_epoch_seconds) {
    const std::string start_text = FormatSpokenTime(starts_at, now_epoch_seconds);
    if (ends_at.empty()) return start_text;

    const int64_t start_epoch = ParseIso8601(starts_at);
    const int64_t end_epoch = ParseIso8601(ends_at);
    std::tm start{};
    std::tm end{};
    std::tm now{};
    if (!ShanghaiTime(start_epoch, &start) || !ShanghaiTime(end_epoch, &end) ||
        !ShanghaiTime(now_epoch_seconds, &now)) {
        return start_text;
    }

    if (ShanghaiDay(start_epoch) != ShanghaiDay(end_epoch)) {
        return start_text + "到" + FormatSpokenTime(ends_at, now_epoch_seconds);
    }

    std::string end_text;
    if (std::string(SpokenPeriod(start.tm_hour)) != SpokenPeriod(end.tm_hour)) {
        end_text = SpokenPeriod(end.tm_hour);
    }
    end_text += SpokenClock(end);
    return start_text + "到" + end_text;
}

int Iso8601LocalWeekday(const std::string& value) {
    ParsedIso8601 parsed;
    if (!ParseIso8601Parts(value, &parsed)) return 0;
    const int64_t days = DaysFromCivil(parsed.year, static_cast<unsigned>(parsed.month),
                                       static_cast<unsigned>(parsed.day));
    return IsoWeekdayFromCivilDay(days);
}

int Iso8601LocalMonthDay(const std::string& value) {
    ParsedIso8601 parsed;
    return ParseIso8601Parts(value, &parsed) ? parsed.day : 0;
}

std::string AlignWeeklyStartAt(const std::string& proposed_start_at, int target_weekday,
                               int64_t now_epoch_seconds) {
    ParsedIso8601 proposed;
    if (target_weekday < 1 || target_weekday > 7 || now_epoch_seconds < 0 ||
        !ParseIso8601Parts(proposed_start_at, &proposed)) {
        return {};
    }

    const std::time_t local_now =
        static_cast<std::time_t>(now_epoch_seconds + proposed.offset_minutes * 60LL);
    std::tm now{};
    if (gmtime_r(&local_now, &now) == nullptr) return {};
    const int64_t today = DaysFromCivil(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    const int today_weekday = IsoWeekdayFromCivilDay(today);
    int days_ahead = (target_weekday - today_weekday + 7) % 7;
    int64_t candidate_local = (today + days_ahead) * 86400LL + proposed.hour * 3600LL +
                              proposed.minute * 60LL + proposed.second;
    int64_t candidate_epoch = candidate_local - proposed.offset_minutes * 60LL;
    if (candidate_epoch < now_epoch_seconds - 1) candidate_epoch += 7 * 86400LL;
    return FormatAtOffset(candidate_epoch, proposed.offset_minutes);
}

std::string NextOccurrenceUtc(const Event& event, const std::string& current_start_at) {
    if (event.recurrence_frequency.empty() || event.terminated) return {};
    const int64_t current = ParseIso8601(current_start_at);
    if (current < 0) return {};
    if (event.recurrence_frequency == "daily") return FormatUtc(current + 86400LL);
    if (event.recurrence_frequency == "weekly") return FormatUtc(current + 7 * 86400LL);
    if (event.recurrence_frequency != "monthly") return {};

    ParsedIso8601 original;
    if (!ParseIso8601Parts(event.starts_at, &original)) return {};
    const int64_t current_local_epoch = current + original.offset_minutes * 60LL;
    std::time_t raw = static_cast<std::time_t>(current_local_epoch);
    std::tm current_local{};
    if (gmtime_r(&raw, &current_local) == nullptr) return {};

    int year = current_local.tm_year + 1900;
    int month = current_local.tm_mon + 1;
    const int requested_day = event.recurrence_month_day > 0
        ? event.recurrence_month_day
        : original.day;
    for (int attempt = 0; attempt < 24; ++attempt) {
        if (++month > 12) {
            month = 1;
            ++year;
        }
        if (requested_day > DaysInMonth(year, month)) continue;
        const int64_t local_seconds =
            DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(requested_day)) * 86400LL +
            original.hour * 3600LL + original.minute * 60LL + original.second;
        return FormatUtc(local_seconds - original.offset_minutes * 60LL);
    }
    return {};
}

std::string NewId(const char* prefix) {
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%s-%08lx%08lx",
                  prefix == nullptr ? "id" : prefix,
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));
    return buffer;
}

bool IsSensitiveNote(const std::string& content) {
    std::string folded;
    folded.reserve(content.size());
    for (unsigned char c : content) folded.push_back(static_cast<char>(std::tolower(c)));
    static constexpr const char* needles[] = {
        "密码", "验证码", "取件码", "支付密码", "银行卡", "私钥", "api key", "apikey", "token", "secret",
    };
    for (const char* needle : needles) {
        if (folded.find(needle) != std::string::npos) return true;
    }
    return false;
}

const char* ReminderStatusName(ReminderStatus status) {
    switch (status) {
        case ReminderStatus::Scheduled: return "scheduled";
        case ReminderStatus::Snoozed: return "snoozed";
        case ReminderStatus::Pushed: return "pushed";
        case ReminderStatus::Closed: return "closed";
    }
    return "scheduled";
}

ReminderStatus ReminderStatusFromName(const std::string& name) {
    if (name == "snoozed") return ReminderStatus::Snoozed;
    if (name == "pushed") return ReminderStatus::Pushed;
    if (name == "closed") return ReminderStatus::Closed;
    return ReminderStatus::Scheduled;
}

}  // namespace voicelife
