#include "voicelife/im/im_reporting_channel.h"

#include <string>
#include <utility>

#include "im_wire.h"

namespace voicelife::im {
namespace {

constexpr const char* kScheduleReceiptPath = "/v1/im/schedule-receipts";
constexpr const char* kNotificationPath = "/v1/im/notifications";

}  // namespace

ImReportingChannel::ImReportingChannel(ImTransport& transport, ImCredentialProvider& credentials)
    : transport_(transport), credentials_(credentials) {}

ReportResult ImReportingChannel::SubmitScheduleReceipt(const contracts::im::ScheduleReceiptIntent& intent) {
    return Submit(kScheduleReceiptPath, intent.eventId, intent.deviceId, SerializeScheduleReceiptIntent(intent));
}

ReportResult ImReportingChannel::SubmitNotification(const contracts::im::NotificationIntent& intent) {
    return Submit(kNotificationPath, intent.businessEventId, intent.recipient.deviceId,
                  SerializeNotificationIntent(intent));
}

ReportResult ImReportingChannel::Submit(const std::string& path, const std::string& idempotency_key,
                                        const std::string& intent_device_id, const std::string& body) {
    const std::string token = credentials_.DeviceToken();
    const std::string device_id = credentials_.DeviceId();
    if (token.empty()) {
        return {ReportStatus::kCredentialRejected, "设备凭据未配置"};
    }
    if (device_id.empty() || device_id != intent_device_id) {
        return {ReportStatus::kCredentialRejected, "deviceId 与意图不一致"};
    }

    ImHttpRequest request;
    request.path = path;
    request.method = "POST";
    request.body = body;
    request.headers = {{"Content-Type", "application/json"},
                       {"Authorization", "Bearer " + token},
                       {"Idempotency-Key", idempotency_key}};

    const ImHttpResponse response = transport_.Post(request);
    switch (response.status) {
        case ImTransportStatus::kSuccess:
            return {ReportStatus::kSubmitted, response.message};
        case ImTransportStatus::kCredentialRejected:
            return {ReportStatus::kCredentialRejected, response.message};
        case ImTransportStatus::kHttpError:
        case ImTransportStatus::kNetworkFailure:
            return {ReportStatus::kRetryable, response.message};
    }
    return {ReportStatus::kRetryable, "未知传输结果"};
}

}  // namespace voicelife::im
