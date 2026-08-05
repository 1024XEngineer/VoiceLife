#include "voicelife/contracts/im/schedule_receipt.h"

#include <string>
#include <string_view>
#include <utility>

#include "contract_parsing.h"

namespace voicelife::contracts::im {
namespace {

using detail::OptionalString;
using detail::Reject;
using detail::RequireEnum;
using detail::RequireIsoDateTime;
using detail::RequireString;

}  // namespace

Status ParseScheduleReceiptIntent(const JsonValue& root, ScheduleReceiptIntent& out) {
    ScheduleReceiptIntent parsed;
    if (!root.IsObject()) {
        return Reject("ScheduleReceiptIntent 必须是对象");
    }
    const JsonValue* schema_version = root.Get("schemaVersion");
    if (schema_version == nullptr || !schema_version->IsString() || schema_version->string != kDeviceContractVersion) {
        return Reject("schemaVersion 必须等于 1");
    }
    parsed.schemaVersion = kDeviceContractVersion;

    if (const Status status = RequireString(root, "eventId", parsed.eventId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "correlationId", parsed.correlationId); !status.ok()) {
        return status;
    }
    if (const Status status = OptionalString(root, "userId", parsed.userId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "deviceId", parsed.deviceId); !status.ok()) {
        return status;
    }
    if (const Status status =
            RequireEnum(root, "operationType", {"created", "updated", "cancelled", "undone"}, parsed.operationType);
        !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "scheduleId", parsed.scheduleId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireEnum(root, "result", {"succeeded", "failed"}, parsed.result); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "summary", parsed.summary); !status.ok()) {
        return status;
    }
    if (const Status status = RequireIsoDateTime(root, "occurredAt", parsed.occurredAt); !status.ok()) {
        return status;
    }
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife::contracts::im
