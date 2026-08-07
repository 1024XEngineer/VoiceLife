#include "voicelife/contracts/im/reminder_action_command.h"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "contract_parsing.h"

namespace voicelife::contracts::im {
namespace {

using detail::Reject;
using detail::RequireEnum;
using detail::RequireIsoDateTime;
using detail::RequireString;

inline constexpr int kMaxSnoozeMinutes = 24 * 60;

[[nodiscard]] Status ParseReminderActionCommandValue(const JsonValue& root, ReminderActionCommand& out) {
    if (!root.IsObject()) {
        return Reject("ReminderActionCommand 必须是对象");
    }
    const JsonValue* schema_version = root.Get("schemaVersion");
    if (schema_version == nullptr || !schema_version->IsString() || schema_version->string != kDeviceContractVersion) {
        return Reject("schemaVersion 必须等于 1");
    }
    out.schemaVersion = kDeviceContractVersion;

    if (const Status status = RequireString(root, "commandId", out.commandId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "operationId", out.operationId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "correlationId", out.correlationId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "deviceId", out.deviceId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "actorBindingId", out.actorBindingId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "reminderTriggerId", out.reminderTriggerId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireEnum(root, "action", {"acknowledge", "snooze"}, out.action); !status.ok()) {
        return status;
    }
    const JsonValue* params = root.Get("params");
    if (params != nullptr) {
        if (!params->IsObject()) {
            return Reject("动作 params 必须是对象");
        }
        const JsonValue* minutes = params->Get("minutes");
        if (minutes == nullptr || minutes->kind != JsonValue::Kind::kNumber || minutes->number <= 0 ||
            std::floor(minutes->number) != minutes->number ||
            minutes->number > static_cast<double>(std::numeric_limits<int>::max()) ||
            minutes->number > static_cast<double>(kMaxSnoozeMinutes)) {
            return Reject("动作 params.minutes 必须是 1 到 1440 的整数");
        }
        out.minutes = static_cast<int>(minutes->number);
    }
    if (out.action == "acknowledge" && params != nullptr) {
        return Reject("acknowledge 命令不得携带 params");
    }
    if (out.action == "snooze" && !out.minutes.has_value()) {
        return Reject("snooze 命令必须携带 minutes");
    }
    if (const Status status = RequireIsoDateTime(root, "occurredAt", out.occurredAt); !status.ok()) {
        return status;
    }
    return RequireIsoDateTime(root, "expiresAt", out.expiresAt);
}

}  // namespace

Status ParseReminderActionCommand(const JsonValue& root, ReminderActionCommand& out) {
    ReminderActionCommand parsed;
    if (const Status status = ParseReminderActionCommandValue(root, parsed); !status.ok()) {
        return status;
    }
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife::contracts::im
