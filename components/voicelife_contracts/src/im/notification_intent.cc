#include "voicelife/contracts/im/notification_intent.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "contract_parsing.h"

namespace voicelife::contracts::im {
namespace {

using detail::Reject;
using detail::RequireString;

/// 单个通知意图允许的最大动作数，避免超限输入放大内存与执行成本。
inline constexpr size_t kMaxActions = 16;

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
    return detail::OptionalString(*content, "body", out.body);
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
            std::floor(minutes->number) != minutes->number ||
            minutes->number > static_cast<double>(std::numeric_limits<int>::max())) {
            return Reject("动作 params.minutes 必须是 int 范围内的正整数");
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
    NotificationIntent parsed;
    if (!root.IsObject()) {
        return Reject("NotificationIntent 必须是对象");
    }
    const JsonValue* schema_version = root.Get("schemaVersion");
    if (schema_version == nullptr || !schema_version->IsString() || schema_version->string != kDeviceContractVersion) {
        return Reject("schemaVersion 必须等于 1");
    }
    parsed.schemaVersion = kDeviceContractVersion;

    const JsonValue* reminder_type = root.Get("reminderType");
    if (reminder_type == nullptr || !reminder_type->IsString() ||
        (reminder_type->string != "weak" && reminder_type->string != "strong")) {
        return Reject("reminderType 必须是 weak 或 strong");
    }
    parsed.reminderType = reminder_type->string;

    const JsonValue* actions = root.Get("actions");
    if (actions == nullptr || !actions->IsArray()) {
        return Reject("actions 必须是数组");
    }
    if (parsed.reminderType == "weak" && !actions->array.empty()) {
        return Reject("弱提醒的 actions 必须为空");
    }
    if (parsed.reminderType == "strong" && actions->array.empty()) {
        return Reject("强提醒的 actions 必须包含至少一个动作");
    }
    if (actions->array.size() > kMaxActions) {
        return Reject("actions 数量超出上限");
    }
    for (const JsonValue& action : actions->array) {
        NotificationAction parsed_action;
        if (const Status status = ParseNotificationAction(action, parsed_action); !status.ok()) {
            return status;
        }
        parsed.actions.push_back(std::move(parsed_action));
    }

    if (const Status status = RequireString(root, "businessEventId", parsed.businessEventId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "correlationId", parsed.correlationId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "kind", parsed.kind); !status.ok()) {
        return status;
    }
    if (parsed.kind != "reminder_due") {
        return Reject("kind 必须是 reminder_due");
    }
    if (const Status status = ParseRecipient(root, parsed.recipient); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "scheduleId", parsed.scheduleId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "taskId", parsed.taskId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "instanceId", parsed.instanceId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "reminderTriggerId", parsed.reminderTriggerId); !status.ok()) {
        return status;
    }
    if (const Status status = ParseContent(root, parsed.content); !status.ok()) {
        return status;
    }
    if (const Status status = detail::RequireIsoDateTime(root, "plannedAt", parsed.plannedAt); !status.ok()) {
        return status;
    }
    if (const Status status = detail::RequireIsoDateTime(root, "triggerAt", parsed.triggerAt); !status.ok()) {
        return status;
    }
    if (const Status status = detail::RequireIsoDateTime(root, "occurredAt", parsed.occurredAt); !status.ok()) {
        return status;
    }
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife::contracts::im
