#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

// 输入已经通过 IsValidIsoDateTime 后，将含时区的时间归一化为 Unix 毫秒，供跨字段顺序校验。
inline std::optional<int64_t> IsoDateTimeMillis(const std::string& input) {
    if (!IsValidIsoDateTime(input)) return std::nullopt;
    auto number = [&](size_t offset, size_t count) {
        int value = 0;
        for (size_t index = 0; index < count; ++index) value = value * 10 + (input[offset + index] - '0');
        return value;
    };
    int year = number(0, 4);
    const unsigned month = static_cast<unsigned>(number(5, 2));
    const unsigned day = static_cast<unsigned>(number(8, 2));
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned shifted_month = static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
    const unsigned day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
    int64_t seconds = days * 86400 + number(11, 2) * 3600 + number(14, 2) * 60 + number(17, 2);
    size_t pos = 19;
    int64_t milliseconds = 0;
    if (input[pos] == '.') {
        ++pos;
        unsigned fraction_digits = 0;
        while (input[pos] >= '0' && input[pos] <= '9') {
            if (fraction_digits < 3) milliseconds = milliseconds * 10 + (input[pos] - '0');
            ++fraction_digits;
            ++pos;
        }
        while (fraction_digits < 3) {
            milliseconds *= 10;
            ++fraction_digits;
        }
    }
    if (input[pos] != 'Z') {
        const int offset = number(pos + 1, 2) * 60 + number(pos + 4, 2);
        seconds -= (input[pos] == '+' ? offset : -offset) * 60;
    }
    return seconds * 1000 + milliseconds;
}

inline bool HasOnlyFields(const JsonValue& value, std::initializer_list<std::string_view> allowed) {
    if (!value.IsObject()) return false;
    for (const auto& [key, child] : value.object) {
        (void)child;
        bool matched = false;
        for (const std::string_view candidate : allowed) {
            if (key == candidate) {
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
}

inline Status RequireIsoDateTime(const JsonValue& root, const char* key, std::string& out) {
    const JsonValue* value = root.Get(key);
    if (value == nullptr || !value->IsString() || !IsValidIsoDateTime(value->string)) {
        return Reject("时间字段必须是合法 ISO 8601");
    }
    out = value->string;
    return Status::Ok();
}

inline Status OptionalIsoDateTime(const JsonValue& root, const char* key, std::optional<std::string>& out) {
    const JsonValue* value = root.Get(key);
    if (value == nullptr) {
        return Status::Ok();
    }
    if (!value->IsString() || !IsValidIsoDateTime(value->string)) {
        return Reject("可选时间字段必须是合法 ISO 8601");
    }
    out = value->string;
    return Status::Ok();
}

inline bool FitsOptionalJsonBudget(const JsonValue& value, size_t depth, size_t& nodes) {
    constexpr size_t kMaxDepth = 4;
    constexpr size_t kMaxNodes = 64;
    constexpr size_t kMaxContainerItems = 16;
    constexpr size_t kMaxStringBytes = 1024;
    if (depth > kMaxDepth || ++nodes > kMaxNodes) {
        return false;
    }
    if (value.kind == JsonValue::Kind::kString) {
        return value.string.size() <= kMaxStringBytes;
    }
    if (value.kind == JsonValue::Kind::kArray) {
        if (value.array.size() > kMaxContainerItems) {
            return false;
        }
        for (const JsonValue& item : value.array) {
            if (!FitsOptionalJsonBudget(item, depth + 1, nodes)) {
                return false;
            }
        }
    }
    if (value.kind == JsonValue::Kind::kObject) {
        if (value.object.size() > kMaxContainerItems) {
            return false;
        }
        for (const auto& [item_key, item] : value.object) {
            if (item_key.size() > kMaxStringBytes || !FitsOptionalJsonBudget(item, depth + 1, nodes)) {
                return false;
            }
        }
    }
    return true;
}

inline Status OptionalJsonValue(const JsonValue& root, const char* key, std::optional<JsonValue>& out) {
    const JsonValue* value = root.Get(key);
    if (value == nullptr) {
        return Status::Ok();
    }
    size_t nodes = 0;
    if (!FitsOptionalJsonBudget(*value, 0, nodes)) {
        return Reject("可选 JSON 字段超出资源预算");
    }
    out = *value;
    return Status::Ok();
}

}  // namespace detail
}  // namespace voicelife::contracts::im
