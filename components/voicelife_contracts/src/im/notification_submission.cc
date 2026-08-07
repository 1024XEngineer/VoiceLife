#include "voicelife/contracts/im/notification_submission.h"

#include <string>
#include <utility>

#include "contract_parsing.h"

namespace voicelife::contracts::im {
namespace {

using detail::Reject;
using detail::RequireEnum;
using detail::RequireIsoDateTime;
using detail::RequireString;

[[nodiscard]] Status ParseDelivery(const JsonValue& value, NotificationDelivery& out) {
    if (!value.IsObject()) {
        return Reject("交付行必须是对象");
    }
    if (const Status status = RequireString(value, "deliveryId", out.deliveryId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireString(value, "bindingId", out.bindingId); !status.ok()) {
        return status;
    }
    return RequireEnum(value, "status", {"pending"}, out.status);
}

[[nodiscard]] Status ParseActionStream(const JsonValue& root, std::optional<NotificationActionStream>& out) {
    const JsonValue* stream = root.Get("actionStream");
    if (stream == nullptr) {
        return Status::Ok();
    }
    if (!stream->IsObject()) {
        return Reject("actionStream 必须是对象");
    }
    NotificationActionStream parsed;
    if (const Status status = RequireString(*stream, "reminderTriggerId", parsed.reminderTriggerId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireIsoDateTime(*stream, "expiresAt", parsed.expiresAt); !status.ok()) {
        return status;
    }
    out = std::move(parsed);
    return Status::Ok();
}

[[nodiscard]] Status ParseNotificationSubmissionValue(const JsonValue& root, NotificationSubmission& out) {
    if (!root.IsObject()) {
        return Reject("NotificationSubmission 必须是对象");
    }
    if (const Status status = RequireString(root, "businessEventId", out.businessEventId); !status.ok()) {
        return status;
    }
    if (const Status status = RequireEnum(root, "status", {"accepted"}, out.status); !status.ok()) {
        return status;
    }
    const JsonValue* deliveries = root.Get("deliveries");
    if (deliveries == nullptr || !deliveries->IsArray()) {
        return Reject("deliveries 必须是数组");
    }
    for (const JsonValue& delivery : deliveries->array) {
        NotificationDelivery parsed;
        if (const Status status = ParseDelivery(delivery, parsed); !status.ok()) {
            return status;
        }
        out.deliveries.push_back(std::move(parsed));
    }
    return ParseActionStream(root, out.actionStream);
}

}  // namespace

Status ParseNotificationSubmission(const JsonValue& root, NotificationSubmission& out) {
    NotificationSubmission parsed;
    if (const Status status = ParseNotificationSubmissionValue(root, parsed); !status.ok()) {
        return status;
    }
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife::contracts::im
