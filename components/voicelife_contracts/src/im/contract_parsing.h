#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {
namespace detail {

inline Status Reject(const char* message) { return Status::Error(ErrorCode::kInvalidArgument, message); }

inline Status RequireString(const JsonValue& root, const char* key, std::string& out) {
    const JsonValue* value = root.Get(key);
    if (value == nullptr || !value->IsString() || value->string.empty()) {
        return Reject("缺少非空字符串字段");
    }
    out = value->string;
    return Status::Ok();
}

inline Status OptionalString(const JsonValue& root, const char* key, std::optional<std::string>& out) {
    const JsonValue* value = root.Get(key);
    if (value == nullptr) {
        return Status::Ok();
    }
    if (!value->IsString() || value->string.empty()) {
        return Reject("可选字符串字段必须非空");
    }
    out = value->string;
    return Status::Ok();
}

inline Status RequireEnum(const JsonValue& root, const char* key, std::initializer_list<std::string_view> allowed,
                          std::string& out) {
    if (const Status status = RequireString(root, key, out); !status.ok()) {
        return status;
    }
    for (const std::string_view candidate : allowed) {
        if (out == candidate) {
            return Status::Ok();
        }
    }
    return Reject("枚举字段取值非法");
}

// 严格校验 ISO 8601 日期时间：YYYY-MM-DDTHH:MM:SS(.frac)?(Z|±HH:MM)。
inline bool IsValidIsoDateTime(const std::string& input) {
    size_t pos = 0;
    auto read_digits = [&](size_t count) -> std::optional<int> {
        if (pos + count > input.size()) {
            return std::nullopt;
        }
        int value = 0;
        for (size_t i = 0; i < count; ++i) {
            const char current = input[pos + i];
            if (current < '0' || current > '9') {
                return std::nullopt;
            }
            value = value * 10 + (current - '0');
        }
        pos += count;
        return value;
    };
    auto expect = [&](char expected) -> bool {
        if (pos >= input.size() || input[pos] != expected) {
            return false;
        }
        ++pos;
        return true;
    };

    const auto year = read_digits(4);
    if (!year.has_value() || !expect('-')) {
        return false;
    }
    const auto month = read_digits(2);
    if (!month.has_value() || !expect('-')) {
        return false;
    }
    const auto day = read_digits(2);
    if (!day.has_value() || !expect('T')) {
        return false;
    }
    const auto hour = read_digits(2);
    if (!hour.has_value() || !expect(':')) {
        return false;
    }
    const auto minute = read_digits(2);
    if (!minute.has_value() || !expect(':')) {
        return false;
    }
    const auto second = read_digits(2);
    if (!second.has_value()) {
        return false;
    }
    if (pos < input.size() && input[pos] == '.') {
        ++pos;
        size_t fraction_digits = 0;
        while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') {
            ++pos;
            ++fraction_digits;
        }
        if (fraction_digits < 1 || fraction_digits > 9) {
            return false;
        }
    }
    int offset_hour = 0;
    int offset_minute = 0;
    if (pos < input.size() && input[pos] == 'Z') {
        ++pos;
    } else if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) {
        ++pos;
        offset_hour = read_digits(2).value_or(-1);
        if (offset_hour < 0 || !expect(':')) {
            return false;
        }
        offset_minute = read_digits(2).value_or(-1);
        if (offset_minute < 0) {
            return false;
        }
    } else {
        return false;
    }
    if (pos != input.size() || *month < 1 || *month > 12) {
        return false;
    }
    const bool leap_year = *year % 4 == 0 && (*year % 100 != 0 || *year % 400 == 0);
    constexpr int kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int max_day = (*month == 2 && leap_year) ? 29 : kDaysInMonth[*month - 1];
    return *day >= 1 && *day <= max_day && *hour <= 23 && *minute <= 59 && *second <= 59 && offset_hour <= 23 &&
           offset_minute <= 59;
}

inline Status RequireIsoDateTime(const JsonValue& root, const char* key, std::string& out) {
    const JsonValue* value = root.Get(key);
    if (value == nullptr || !value->IsString() || !IsValidIsoDateTime(value->string)) {
        return Reject("时间字段必须是合法 ISO 8601");
    }
    out = value->string;
    return Status::Ok();
}

}  // namespace detail
}  // namespace voicelife::contracts::im
