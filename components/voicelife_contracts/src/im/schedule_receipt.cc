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
    if (!root.IsObject()) {
        return Reject("ScheduleReceiptIntent 必须是对象");
    }
    const JsonValue* schema_version = root.Get("schemaVersion");
    if (schema_version == nullptr || !schema_version->IsString() || schema_version->string != kDeviceContractVersion) {
        return Reject("schemaVersion 必须等于 1");
    }
    out.schemaVersion = kDeviceContractVersion;

    if (const Status status = RequireString(root, "eventId", out.eventId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "correlationId", out.correlationId); !status.ok()) {
        return status;
    }
    if (const Status status = OptionalString(root, "userId", out.userId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "deviceId", out.deviceId); !status.ok()) {
        return status;
    }
    if (const Status status =
            RequireEnum(root, "operationType", {"created", "updated", "cancelled", "undone"}, out.operationType);
        !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "scheduleId", out.scheduleId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireEnum(root, "result", {"succeeded", "failed"}, out.result); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(root, "summary", out.summary); !status.ok()) {
        return status;
    }
    return RequireIsoDateTime(root, "occurredAt", out.occurredAt);
}

}  // namespace voicelife::contracts::im
