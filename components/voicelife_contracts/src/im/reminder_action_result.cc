#include "voicelife/contracts/im/reminder_action_result.h"

#include <string>
#include <string_view>
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

}  // namespace

Status ParseReminderActionResult(const JsonValue& root, ReminderActionResult& out) {
    if (!root.IsObject()) {
        return Reject("ReminderActionResult 必须是对象");
    }
    const JsonValue* schema_version = root.Get("schemaVersion");
    if (schema_version == nullptr || !schema_version->IsString() || schema_version->string != kDeviceContractVersion) {
        return Reject("schemaVersion 必须等于 1");
    }
    out.schemaVersion = kDeviceContractVersion;

    if (const Status status = RequireString(root, "operationId", out.operationId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "reminderTriggerId", out.reminderTriggerId); !status.ok()) {
        return status;
    }
    if (const Status status =
            RequireEnum(root, "status", {"succeeded", "retryable_failed", "failed", "expired"}, out.status);
        !status.ok()) {
        return status;
    }
    if (const Status status = OptionalIsoDateTime(root, "nextTriggerAt", out.nextTriggerAt); !status.ok()) {
        return status;
    }
    if (const Status status = OptionalString(root, "errorCode", out.errorCode); !status.ok()) {
        return status;
    }
    if (const Status status = OptionalJsonValue(root, "details", out.details); !status.ok()) {
        return status;
    }
    return RequireIsoDateTime(root, "occurredAt", out.occurredAt);
}

}  // namespace voicelife::contracts::im
