#include "im_wire.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace voicelife::im {

std::string EncodePathSegment(std::string_view segment) {
    std::string out;
    out.reserve(segment.size());
    for (const unsigned char ch : segment) {
        // RFC 3986 非保留字符：ALPHA / DIGIT / "-" / "." / "_" / "~"。
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '.' || ch == '_' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof buffer, "%%%02X", ch);
            out += buffer;
        }
    }
    return out;
}

namespace {

using contracts::im::NotificationAction;
using contracts::im::NotificationIntent;
using contracts::im::ReminderActionResult;
using contracts::im::ScheduleQueryResultIntent;
using contracts::im::ScheduleReceiptIntent;

/// 追加一个 JSON 字符串字面量，转义引号、反斜杠与控制字符。
void AppendJsonString(std::string& out, const std::string& value) {
    out.push_back('"');
    for (const char ch : value) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned int>(ch));
                    out += buffer;
                } else {
                    // 非 ASCII UTF-8 字节原样透传。
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

/// 追加一个键，格式为 "name":。键与值同样需要转义，否则含引号/反斜杠/
/// 控制字符的键（如结果 details 的字段名）会生成非法 JSON。
void AppendKey(std::string& out, const std::string& key) {
    AppendJsonString(out, key);
    out.push_back(':');
}

/// 递归追加任意 JSON 值，用于结果 details 等透传字段。
void AppendJsonValue(std::string& out, const voicelife::JsonValue& value) {
    switch (value.kind) {
        case voicelife::JsonValue::Kind::kNull:
            out += "null";
            break;
        case voicelife::JsonValue::Kind::kBool:
            out += value.boolean ? "true" : "false";
            break;
        case voicelife::JsonValue::Kind::kNumber: {
            const double number = value.number;
            // NaN/Inf 不是合法 JSON 数字，序列化为 null 避免生成非法载荷。
            if (!std::isfinite(number)) {
                out += "null";
                break;
            }
            if (number == std::floor(number) && std::abs(number) < 1e15) {
                out += std::to_string(static_cast<long long>(number));
            } else {
                char buffer[32];
                std::snprintf(buffer, sizeof buffer, "%g", number);
                out += buffer;
            }
            break;
        }
        case voicelife::JsonValue::Kind::kString:
            AppendJsonString(out, value.string);
            break;
        case voicelife::JsonValue::Kind::kArray: {
            out.push_back('[');
            bool first = true;
            for (const voicelife::JsonValue& item : value.array) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                AppendJsonValue(out, item);
            }
            out.push_back(']');
            break;
        }
        case voicelife::JsonValue::Kind::kObject: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, item] : value.object) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                AppendKey(out, key);
                AppendJsonValue(out, item);
            }
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

std::string SerializeScheduleReceiptIntent(const ScheduleReceiptIntent& intent) {
    std::string out;
    out.reserve(256);
    out.push_back('{');
    AppendKey(out, "schemaVersion");
    AppendJsonString(out, intent.schemaVersion);
    out.push_back(',');
    AppendKey(out, "eventId");
    AppendJsonString(out, intent.eventId);
    out.push_back(',');
    AppendKey(out, "correlationId");
    AppendJsonString(out, intent.correlationId);
    if (intent.userId.has_value()) {
        out.push_back(',');
        AppendKey(out, "userId");
        AppendJsonString(out, *intent.userId);
    }
    out.push_back(',');
    AppendKey(out, "deviceId");
    AppendJsonString(out, intent.deviceId);
    out.push_back(',');
    AppendKey(out, "operationType");
    AppendJsonString(out, intent.operationType);
    out.push_back(',');
    AppendKey(out, "scheduleId");
    AppendJsonString(out, intent.scheduleId);
    out.push_back(',');
    AppendKey(out, "result");
    AppendJsonString(out, intent.result);
    out.push_back(',');
    AppendKey(out, "summary");
    AppendJsonString(out, intent.summary);
    out.push_back(',');
    AppendKey(out, "occurredAt");
    AppendJsonString(out, intent.occurredAt);
    out.push_back('}');
    return out;
}

std::string SerializeScheduleQueryResultIntent(const ScheduleQueryResultIntent& intent) {
    std::string out;
    out.reserve(2048);
    out.push_back('{');
    AppendKey(out, "schemaVersion");
    AppendJsonString(out, intent.schemaVersion);
    out.push_back(',');
    AppendKey(out, "businessEventId");
    AppendJsonString(out, intent.businessEventId);
    out.push_back(',');
    AppendKey(out, "correlationId");
    AppendJsonString(out, intent.correlationId);
    if (intent.userId.has_value()) {
        out += ",\"userId\":";
        AppendJsonString(out, *intent.userId);
    }
    out += ",\"deviceId\":";
    AppendJsonString(out, intent.deviceId);
    out += ",\"query\":{\"status\":";
    AppendJsonString(out, intent.status);
    if (intent.keyword.has_value()) {
        out += ",\"keyword\":";
        AppendJsonString(out, *intent.keyword);
    }
    if (intent.startDate.has_value()) {
        out += ",\"startDate\":";
        AppendJsonString(out, *intent.startDate);
    }
    if (intent.endDate.has_value()) {
        out += ",\"endDate\":";
        AppendJsonString(out, *intent.endDate);
    }
    out += "},\"resultCount\":" + std::to_string(intent.resultCount);
    out += ",\"schedules\":";
    AppendJsonValue(out, intent.schedules);
    out += ",\"futureOccurrences\":";
    AppendJsonValue(out, intent.futureOccurrences);
    out += ",\"exceptions\":";
    AppendJsonValue(out, intent.exceptions);
    out += ",\"queriedAt\":";
    AppendJsonString(out, intent.queriedAt);
    out.push_back('}');
    return out;
}

std::string SerializeNotificationIntent(const NotificationIntent& intent) {
    std::string out;
    out.reserve(512);
    out.push_back('{');
    AppendKey(out, "schemaVersion");
    AppendJsonString(out, intent.schemaVersion);
    out.push_back(',');
    AppendKey(out, "businessEventId");
    AppendJsonString(out, intent.businessEventId);
    out.push_back(',');
    AppendKey(out, "correlationId");
    AppendJsonString(out, intent.correlationId);
    out.push_back(',');
    AppendKey(out, "kind");
    AppendJsonString(out, intent.kind);
    out.push_back(',');
    AppendKey(out, "recipient");
    out += "{\"userId\":";
    AppendJsonString(out, intent.recipient.userId);
    out += ",\"deviceId\":";
    AppendJsonString(out, intent.recipient.deviceId);
    out.push_back('}');
    out.push_back(',');
    AppendKey(out, "scheduleId");
    AppendJsonString(out, intent.scheduleId);
    out.push_back(',');
    AppendKey(out, "taskId");
    AppendJsonString(out, intent.taskId);
    out.push_back(',');
    AppendKey(out, "instanceId");
    AppendJsonString(out, intent.instanceId);
    out.push_back(',');
    AppendKey(out, "reminderTriggerId");
    AppendJsonString(out, intent.reminderTriggerId);
    out.push_back(',');
    AppendKey(out, "reminderType");
    AppendJsonString(out, intent.reminderType);
    out.push_back(',');
    AppendKey(out, "content");
    out += "{\"title\":";
    AppendJsonString(out, intent.content.title);
    if (intent.content.body.has_value()) {
        out += ",\"body\":";
        AppendJsonString(out, *intent.content.body);
    }
    out.push_back('}');
    out.push_back(',');
    AppendKey(out, "plannedAt");
    AppendJsonString(out, intent.plannedAt);
    out.push_back(',');
    AppendKey(out, "triggerAt");
    AppendJsonString(out, intent.triggerAt);
    out.push_back(',');
    AppendKey(out, "actions");
    out += "[";
    bool first = true;
    for (const NotificationAction& action : intent.actions) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += "{\"kind\":";
        AppendJsonString(out, action.kind);
        out += ",\"type\":";
        AppendJsonString(out, action.type);
        out += ",\"label\":";
        AppendJsonString(out, action.label);
        if (action.minutes.has_value()) {
            out += ",\"params\":{\"minutes\":";
            out += std::to_string(*action.minutes);
            out += "}";
        }
        out.push_back('}');
    }
    out += "]";
    out.push_back(',');
    AppendKey(out, "occurredAt");
    AppendJsonString(out, intent.occurredAt);
    out.push_back('}');
    return out;
}

std::string SerializeReminderActionResult(const ReminderActionResult& result) {
    std::string out;
    out.reserve(256);
    out.push_back('{');
    AppendKey(out, "schemaVersion");
    AppendJsonString(out, result.schemaVersion);
    out.push_back(',');
    AppendKey(out, "operationId");
    AppendJsonString(out, result.operationId);
    out.push_back(',');
    AppendKey(out, "reminderTriggerId");
    AppendJsonString(out, result.reminderTriggerId);
    out.push_back(',');
    AppendKey(out, "status");
    AppendJsonString(out, result.status);
    if (result.nextTriggerAt.has_value()) {
        out.push_back(',');
        AppendKey(out, "nextTriggerAt");
        AppendJsonString(out, *result.nextTriggerAt);
    }
    if (result.errorCode.has_value()) {
        out.push_back(',');
        AppendKey(out, "errorCode");
        AppendJsonString(out, *result.errorCode);
    }
    if (result.details.has_value()) {
        out.push_back(',');
        AppendKey(out, "details");
        AppendJsonValue(out, *result.details);
    }
    out.push_back(',');
    AppendKey(out, "occurredAt");
    AppendJsonString(out, result.occurredAt);
    out.push_back('}');
    return out;
}

std::string SerializeVoiceReminderActionStatus(const contracts::im::VoiceReminderActionStatus& status) {
    std::string out;
    out.reserve(384);
    out += "{\"schemaVersion\":";
    AppendJsonString(out, status.schemaVersion);
    out += ",\"eventId\":";
    AppendJsonString(out, status.eventId);
    out += ",\"correlationId\":";
    AppendJsonString(out, status.correlationId);
    out += ",\"deviceId\":";
    AppendJsonString(out, status.deviceId);
    out += ",\"reminderTriggerId\":";
    AppendJsonString(out, status.reminderTriggerId);
    out += ",\"operationId\":";
    AppendJsonString(out, status.operationId);
    out += ",\"action\":";
    AppendJsonString(out, status.action);
    out += ",\"status\":";
    AppendJsonString(out, status.status);
    out += ",\"occurredAt\":";
    AppendJsonString(out, status.occurredAt);
    if (status.nextTriggerAt.has_value()) {
        out += ",\"nextTriggerAt\":";
        AppendJsonString(out, *status.nextTriggerAt);
    }
    out += ",\"source\":";
    AppendJsonString(out, status.source);
    out.push_back('}');
    return out;
}

}  // namespace voicelife::im
