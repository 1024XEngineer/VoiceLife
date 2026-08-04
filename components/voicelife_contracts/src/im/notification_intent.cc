#include "voicelife/contracts/im/notification_intent.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace voicelife::contracts::im {
namespace {

[[nodiscard]] Status Reject(const char* message) { return Status::Error(ErrorCode::kInvalidArgument, message); }

[[nodiscard]] Status RequireString(const JsonValue& root, const char* key, std::string& out) {
    const JsonValue* value = root.Get(key);
    if (value == nullptr || !value->IsString() || value->string.empty()) {
        return Reject("缺少非空字符串字段");
    }
    out = value->string;
    return Status::Ok();
}

[[nodiscard]] Status OptionalString(const JsonValue& root, const char* key, std::optional<std::string>& out) {
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

// 严格校验 ISO 8601 日期时间：YYYY-MM-DDTHH:MM:SS(.frac)?(Z|±HH:MM)。
[[nodiscard]] bool IsValidIsoDateTime(const std::string& input) {
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

[[nodiscard]] Status RequireIsoDateTime(const JsonValue& root, const char* key, std::string& out) {
    const JsonValue* value = root.Get(key);
    if (value == nullptr || !value->IsString() || !IsValidIsoDateTime(value->string)) {
        return Reject("时间字段必须是合法 ISO 8601");
    }
    out = value->string;
    return Status::Ok();
}

[[nodiscard]] Status ParseRecipient(const JsonValue& root, NotificationRecipient& out) {
    const JsonValue* recipient = root.Get("recipient");
    if (recipient == nullptr || !recipient->IsObject()) {
        return Reject("recipient 必须是对象");
    }
    if (const Status status = RequireString(*recipient, "userId", out.userId); !status.ok()) {
        return status;
    }
    return RequireString(*recipient, "deviceId", out.deviceId);
}

[[nodiscard]] Status ParseContent(const JsonValue& root, NotificationContent& out) {
    const JsonValue* content = root.Get("content");
    if (content == nullptr || !content->IsObject()) {
        return Reject("content 必须是对象");
    }
    if (const Status status = RequireString(*content, "title", out.title); !status.ok()) {
        return status;
    }
    return OptionalString(*content, "body", out.body);
}

[[nodiscard]] Status ParseNotificationAction(const JsonValue& value, NotificationAction& out) {
    if (!value.IsObject()) {
        return Reject("动作必须是对象");
    }
    if (const Status status = RequireString(value, "kind", out.kind); !status.ok()) {
        return status;
    }
    if (out.kind != "command") {
        return Reject("动作 kind 必须是 command");
    }
    if (const Status status = RequireString(value, "type", out.type); !status.ok()) {
        return status;
    }
    if (out.type != "acknowledge" && out.type != "snooze") {
        return Reject("动作 type 必须是 acknowledge 或 snooze");
    }
    if (const Status status = RequireString(value, "label", out.label); !status.ok()) {
        return status;
    }
    const JsonValue* params = value.Get("params");
    if (params != nullptr) {
        if (!params->IsObject()) {
            return Reject("动作 params 必须是对象");
        }
        const JsonValue* minutes = params->Get("minutes");
        if (minutes == nullptr || minutes->kind != JsonValue::Kind::kNumber || minutes->number <= 0 ||
            std::floor(minutes->number) != minutes->number) {
            return Reject("动作 params.minutes 必须是正整数");
        }
        out.minutes = static_cast<int>(minutes->number);
    }
    if (out.type == "snooze" && !out.minutes.has_value()) {
        return Reject("snooze 动作必须携带 minutes");
    }
    return Status::Ok();
}

}  // namespace

Status ParseNotificationIntent(const JsonValue& root, NotificationIntent& out) {
    if (!root.IsObject()) {
        return Reject("NotificationIntent 必须是对象");
    }
    const JsonValue* schema_version = root.Get("schemaVersion");
    if (schema_version == nullptr || !schema_version->IsString() || schema_version->string != kDeviceContractVersion) {
        return Reject("schemaVersion 必须等于 1");
    }
    out.schemaVersion = kDeviceContractVersion;

    const JsonValue* reminder_type = root.Get("reminderType");
    if (reminder_type == nullptr || !reminder_type->IsString() ||
        (reminder_type->string != "weak" && reminder_type->string != "strong")) {
        return Reject("reminderType 必须是 weak 或 strong");
    }
    out.reminderType = reminder_type->string;

    const JsonValue* actions = root.Get("actions");
    if (actions == nullptr || !actions->IsArray()) {
        return Reject("actions 必须是数组");
    }
    if (out.reminderType == "weak" && !actions->array.empty()) {
        return Reject("弱提醒的 actions 必须为空");
    }
    if (out.reminderType == "strong" && actions->array.empty()) {
        return Reject("强提醒的 actions 必须包含至少一个动作");
    }
    for (const JsonValue& action : actions->array) {
        NotificationAction parsed;
        if (const Status status = ParseNotificationAction(action, parsed); !status.ok()) {
            return status;
        }
        out.actions.push_back(std::move(parsed));
    }

    if (const Status status = RequireString(root, "businessEventId", out.businessEventId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "correlationId", out.correlationId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "kind", out.kind); !status.ok()) {
        return status;
    }
    if (out.kind != "reminder_due") {
        return Reject("kind 必须是 reminder_due");
    }
    if (const Status status = ParseRecipient(root, out.recipient); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "scheduleId", out.scheduleId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "taskId", out.taskId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "instanceId", out.instanceId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "reminderTriggerId", out.reminderTriggerId); !status.ok()) {
        return status;
    }
    if (const Status status = ParseContent(root, out.content); !status.ok()) {
        return status;
    }
    if (const Status status = RequireIsoDateTime(root, "plannedAt", out.plannedAt); !status.ok()) {
        return status;
    }
    if (const Status status = RequireIsoDateTime(root, "triggerAt", out.triggerAt); !status.ok()) {
        return status;
    }
    return RequireIsoDateTime(root, "occurredAt", out.occurredAt);
}

}  // namespace voicelife::contracts::im
