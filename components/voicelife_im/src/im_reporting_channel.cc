#include "voicelife/im/im_reporting_channel.h"

#include <string>
#include <utility>

#include "im_wire.h"

namespace voicelife::im {
namespace {

constexpr const char* kScheduleReceiptPath = "/v1/im/schedule-receipts";
constexpr const char* kNotificationPath = "/v1/im/notifications";

/// 发送前契约校验：序列化结果必须能通过网关契约解析，否则本地拒绝。
bool ValidatesAsScheduleReceipt(const std::string& body) {
    voicelife::JsonValue root;
    contracts::im::ScheduleReceiptIntent validated;
    return voicelife::ParseJson(body, root).ok() && contracts::im::ParseScheduleReceiptIntent(root, validated).ok();
}

bool ValidatesAsNotification(const std::string& body) {
    voicelife::JsonValue root;
    contracts::im::NotificationIntent validated;
    return voicelife::ParseJson(body, root).ok() && contracts::im::ParseNotificationIntent(root, validated).ok();
}

}  // namespace

ImReportingChannel::ImReportingChannel(ImTransport& transport, ImCredentialProvider& credentials)
    : transport_(transport), credentials_(credentials) {}

ReportResult ImReportingChannel::SubmitScheduleReceipt(const contracts::im::ScheduleReceiptIntent& intent) {
    const std::string body = SerializeScheduleReceiptIntent(intent);
    if (!ValidatesAsScheduleReceipt(body)) {
        return {ReportStatus::kRejected, "发送前契约校验失败"};
    }
    return Submit(kScheduleReceiptPath, intent.eventId, intent.deviceId, body);
}

ReportResult ImReportingChannel::SubmitNotification(const contracts::im::NotificationIntent& intent) {
    const std::string body = SerializeNotificationIntent(intent);
    if (!ValidatesAsNotification(body)) {
        return {ReportStatus::kRejected, "发送前契约校验失败"};
    }
    return Submit(kNotificationPath, intent.businessEventId, intent.recipient.deviceId, body);
}

ReportResult ImReportingChannel::Submit(const std::string& path, const std::string& idempotency_key,
                                        const std::string& intent_device_id, const std::string& body) {
    const std::string token = credentials_.DeviceToken();
    const std::string device_id = credentials_.DeviceId();
    if (token.empty()) {
        return {ReportStatus::kCredentialRejected, "设备凭据未配置"};
    }
    if (idempotency_key.empty()) {
        return {ReportStatus::kRejected, "幂等键不能为空"};
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
        case ImTransportStatus::kNetworkFailure:
            return {ReportStatus::kRetryable, response.message};
        case ImTransportStatus::kInvalidConfig:
            return {ReportStatus::kRejected, response.message};
        case ImTransportStatus::kHttpError: {
            // 仅超时、限流与服务端 5xx 可重试；其余 4xx/3xx 为明确拒绝。
            const int code = response.status_code;
            if (code == 408 || code == 429 || code >= 500) {
                return {ReportStatus::kRetryable, response.message};
            }
            return {ReportStatus::kRejected, response.message};
        }
    }
    return {ReportStatus::kRetryable, "未知传输结果"};
}

}  // namespace voicelife::im
