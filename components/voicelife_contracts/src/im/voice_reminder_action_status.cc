#include "voicelife/contracts/im/voice_reminder_action_status.h"

#include <utility>

#include "contract_parsing.h"

namespace voicelife::contracts::im {
namespace {

using detail::Reject;
using detail::RequireEnum;
using detail::RequireIsoDateTime;
using detail::RequireString;

Status ParseValue(const JsonValue& root, VoiceReminderActionStatus& out) {
    if (!root.IsObject()) return Reject("VoiceReminderActionStatus 必须是对象");
    const JsonValue* version = root.Get("schemaVersion");
    if (version == nullptr || !version->IsString() || version->string != kDeviceContractVersion) {
        return Reject("schemaVersion 必须等于 1");
    }
    out.schemaVersion = kDeviceContractVersion;
    if (const Status status = RequireString(root, "eventId", out.eventId); !status.ok()) return status;
    if (const Status status = RequireString(root, "correlationId", out.correlationId); !status.ok()) return status;
    if (const Status status = RequireString(root, "deviceId", out.deviceId); !status.ok()) return status;
    if (const Status status = RequireString(root, "reminderTriggerId", out.reminderTriggerId); !status.ok())
        return status;
    if (const Status status = RequireString(root, "operationId", out.operationId); !status.ok()) return status;
    if (const Status status = RequireEnum(root, "action", {"acknowledge", "snooze"}, out.action); !status.ok())
        return status;
    if (const Status status = RequireEnum(root, "status", {"succeeded", "failed"}, out.status); !status.ok())
        return status;
    if (const Status status = RequireIsoDateTime(root, "occurredAt", out.occurredAt); !status.ok()) return status;
    if (const Status status = detail::OptionalIsoDateTime(root, "nextTriggerAt", out.nextTriggerAt); !status.ok())
        return status;
    if (const Status status = RequireEnum(root, "source", {"voice"}, out.source); !status.ok()) return status;
    if (out.action == "snooze" && out.status == "succeeded" && !out.nextTriggerAt.has_value()) {
        return Reject("成功 snooze 必须携带 nextTriggerAt");
    }
    if (out.action == "acknowledge" && out.nextTriggerAt.has_value()) {
        return Reject("acknowledge 不得携带 nextTriggerAt");
    }
    return Status::Ok();
}

}  // namespace

Status ParseVoiceReminderActionStatus(const JsonValue& root, VoiceReminderActionStatus& out) {
    VoiceReminderActionStatus parsed;
    if (const Status status = ParseValue(root, parsed); !status.ok()) return status;
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife::contracts::im
