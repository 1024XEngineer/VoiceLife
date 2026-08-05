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
    ReminderActionResult parsed;
    if (!root.IsObject()) {
        return Reject("ReminderActionResult 必须是对象");
    }
    const JsonValue* schema_version = root.Get("schemaVersion");
    if (schema_version == nullptr || !schema_version->IsString() || schema_version->string != kDeviceContractVersion) {
        return Reject("schemaVersion 必须等于 1");
    }
    parsed.schemaVersion = kDeviceContractVersion;

    if (const Status status = RequireString(root, "operationId", parsed.operationId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "reminderTriggerId", parsed.reminderTriggerId); !status.ok()) {
        return status;
    }
    if (const Status status =
            RequireEnum(root, "status", {"succeeded", "retryable_failed", "failed", "expired"}, parsed.status);
        !status.ok()) {
        return status;
    }
    if (const Status status = OptionalIsoDateTime(root, "nextTriggerAt", parsed.nextTriggerAt); !status.ok()) {
        return status;
    }
    if (const Status status = OptionalString(root, "errorCode", parsed.errorCode); !status.ok()) {
        return status;
    }
    if (const Status status = OptionalJsonValue(root, "details", parsed.details); !status.ok()) {
        return status;
    }
    if (const Status status = RequireIsoDateTime(root, "occurredAt", parsed.occurredAt); !status.ok()) {
        return status;
    }
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife::contracts::im
