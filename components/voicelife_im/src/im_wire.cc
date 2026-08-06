#include "im_wire.h"

#include <cstdio>
#include <string>

namespace voicelife::im {
namespace {

using contracts::im::NotificationAction;
using contracts::im::NotificationIntent;
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

/// 追加一个键，格式为 "name":。
void AppendKey(std::string& out, const std::string& key) {
    out.push_back('"');
    out += key;
    out += "\":";
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

}  // namespace voicelife::im
