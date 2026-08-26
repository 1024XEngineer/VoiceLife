#include "voicelife/contracts/im/reminder_action_status_report.h"

#include <utility>

#include "contract_parsing.h"

namespace voicelife::contracts::im {
namespace {

using detail::OptionalIsoDateTime;
using detail::OptionalJsonValue;
using detail::OptionalString;
using detail::Reject;
using detail::RequireEnum;
using detail::RequireIsoDateTime;
using detail::RequireString;

Status ParseValue(const JsonValue& root, ReminderActionStatusReport& out) {
    if (!root.IsObject()) return Reject("ReminderActionStatusReport 必须是对象");
    const JsonValue* schema = root.Get("schemaVersion");
    if (schema == nullptr || !schema->IsString() || schema->string != kDeviceContractVersion)
        return Reject("schemaVersion 必须等于 1");
    out.schemaVersion = kDeviceContractVersion;
    if (const Status status = RequireString(root, "eventId", out.eventId); !status.ok()) return status;
    if (const Status status = RequireString(root, "correlationId", out.correlationId); !status.ok()) return status;
    if (const Status status = RequireString(root, "deviceId", out.deviceId); !status.ok()) return status;
    if (const Status status = RequireString(root, "reminderTriggerId", out.reminderTriggerId); !status.ok())
        return status;
    if (const Status status = RequireString(root, "operationId", out.operationId); !status.ok()) return status;
    if (const Status status = RequireEnum(root, "action", {"acknowledge", "snooze"}, out.action); !status.ok())
        return status;
    if (const Status status =
            RequireEnum(root, "status", {"succeeded", "retryable_failed", "failed", "expired"}, out.status);
        !status.ok())
        return status;
    if (const Status status = RequireIsoDateTime(root, "occurredAt", out.occurredAt); !status.ok()) return status;
    if (const Status status = OptionalIsoDateTime(root, "nextTriggerAt", out.nextTriggerAt); !status.ok())
        return status;
    if (const Status status = OptionalString(root, "errorCode", out.errorCode); !status.ok()) return status;
    if (const Status status = OptionalJsonValue(root, "details", out.details); !status.ok()) return status;
    if (const Status status = RequireString(root, "source", out.source); !status.ok()) return status;
    if (out.source != "voice") return Reject("source 必须等于 voice");
    if (out.status == "succeeded" && ((out.action == "snooze" && !out.nextTriggerAt.has_value()) ||
                                      (out.action == "acknowledge" && out.nextTriggerAt.has_value()))) {
        return Reject("succeeded 动作的 nextTriggerAt 必须与 action 类型一致");
    }
    return Status::Ok();
}

}  // namespace

Status ParseReminderActionStatusReport(const JsonValue& root, ReminderActionStatusReport& out) {
    ReminderActionStatusReport parsed;
    if (const Status status = ParseValue(root, parsed); !status.ok()) return status;
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife::contracts::im
