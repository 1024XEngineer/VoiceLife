#include "voicelife/contracts/im/schedule_query_result.h"

#include <cmath>
#include <string>
#include <utility>

#include "contract_parsing.h"

namespace voicelife::contracts::im {
namespace {

using detail::OptionalString;
using detail::Reject;
using detail::RequireEnum;
using detail::RequireIsoDateTime;
using detail::RequireString;

Status OptionalIsoDate(const JsonValue& root, const char* key, std::optional<std::string>& out) {
    const JsonValue* value = root.Get(key);
    if (value == nullptr) return Status::Ok();
    if (!value->IsString() || value->string.size() != 10 || value->string[4] != '-' || value->string[7] != '-') {
        return Reject("可选日期字段必须使用 YYYY-MM-DD");
    }
    for (size_t index = 0; index < value->string.size(); ++index) {
        if (index != 4 && index != 7 && (value->string[index] < '0' || value->string[index] > '9')) {
            return Reject("可选日期字段必须使用 YYYY-MM-DD");
        }
    }
    out = value->string;
    return Status::Ok();
}

Status ParseValue(const JsonValue& root, ScheduleQueryResultIntent& out) {
    if (!root.IsObject()) return Reject("ScheduleQueryResultIntent 必须是对象");
    const JsonValue* schema = root.Get("schemaVersion");
    if (schema == nullptr || !schema->IsString() || schema->string != kDeviceContractVersion) {
        return Reject("schemaVersion 必须等于 1");
    }
    out.schemaVersion = kDeviceContractVersion;
    if (const Status status = RequireString(root, "businessEventId", out.businessEventId); !status.ok()) return status;
    if (const Status status = RequireString(root, "correlationId", out.correlationId); !status.ok()) return status;
    if (const Status status = OptionalString(root, "userId", out.userId); !status.ok()) return status;
    if (const Status status = RequireString(root, "deviceId", out.deviceId); !status.ok()) return status;
    const JsonValue* query = root.Get("query");
    if (query == nullptr || !query->IsObject()) return Reject("query 必须是对象");
    if (const Status status = OptionalString(*query, "keyword", out.keyword); !status.ok()) return status;
    if (const Status status = RequireEnum(*query, "status", {"all", "active", "cancelled", "completed"}, out.status);
        !status.ok())
        return status;
    if (const Status status = OptionalIsoDate(*query, "startDate", out.startDate); !status.ok()) return status;
    if (const Status status = OptionalIsoDate(*query, "endDate", out.endDate); !status.ok()) return status;
    const JsonValue* count = root.Get("resultCount");
    if (count == nullptr || count->kind != JsonValue::Kind::kNumber || count->number < 0 || count->number > 1000 ||
        std::floor(count->number) != count->number) {
        return Reject("resultCount 必须是 0 到 1000 的整数");
    }
    out.resultCount = static_cast<int64_t>(count->number);
    const JsonValue* schedules = root.Get("schedules");
    const JsonValue* future = root.Get("futureOccurrences");
    const JsonValue* exceptions = root.Get("exceptions");
    if (schedules == nullptr || !schedules->IsArray()) return Reject("schedules 必须是数组");
    if (future == nullptr || !future->IsArray()) return Reject("futureOccurrences 必须是数组");
    if (exceptions == nullptr || !exceptions->IsArray()) return Reject("exceptions 必须是数组");
    if (out.resultCount != static_cast<int64_t>(schedules->array.size() + future->array.size())) {
        return Reject("resultCount 必须等于 schedules 与 futureOccurrences 数量之和");
    }
    out.schedules = *schedules;
    out.futureOccurrences = *future;
    out.exceptions = *exceptions;
    return RequireIsoDateTime(root, "queriedAt", out.queriedAt);
}

}  // namespace

Status ParseScheduleQueryResultIntent(const JsonValue& root, ScheduleQueryResultIntent& out) {
    ScheduleQueryResultIntent parsed;
    if (const Status status = ParseValue(root, parsed); !status.ok()) return status;
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife::contracts::im
