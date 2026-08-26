#include "voicelife/im/im_reporting_channel.h"

#include <string>
#include <utility>

#include "im_wire.h"

namespace voicelife::im {
namespace {

constexpr const char* kScheduleReceiptPath = "/v1/im/schedule-receipts";
constexpr const char* kScheduleQueryResultPath = "/v1/im/schedule-query-results";
constexpr const char* kNotificationPath = "/v1/im/notifications";
constexpr const char* kReminderActionResultPrefix = "/v1/devices/";
constexpr const char* kReminderActionResultSuffix = "/reminder-actions/";
constexpr const char* kReminderActionResultResultSuffix = "/result";
constexpr const char* kReminderActionStatusPathSuffix = "/reminder-action-status";

/// 发送前契约校验：序列化结果必须能通过网关契约解析，否则本地拒绝。
bool ValidatesAsScheduleReceipt(const std::string& body) {
    voicelife::JsonValue root;
    contracts::im::ScheduleReceiptIntent validated;
    return voicelife::ParseJson(body, root).ok() && contracts::im::ParseScheduleReceiptIntent(root, validated).ok();
}

bool ValidatesAsScheduleQueryResult(const std::string& body) {
    voicelife::JsonValue root;
    contracts::im::ScheduleQueryResultIntent validated;
    voicelife::JsonParseOptions options;
    options.max_bytes = 128 * 1024;
    options.max_nodes = 4096;
    options.max_array_items = 128;
    options.max_allocator_bytes = 512 * 1024;
    return voicelife::ParseJson(body, root, options).ok() &&
           contracts::im::ParseScheduleQueryResultIntent(root, validated).ok();
}

bool ValidatesAsNotification(const std::string& body) {
    voicelife::JsonValue root;
    contracts::im::NotificationIntent validated;
    return voicelife::ParseJson(body, root).ok() && contracts::im::ParseNotificationIntent(root, validated).ok();
}

bool ValidatesAsActionResult(const std::string& body) {
    voicelife::JsonValue root;
    contracts::im::ReminderActionResult validated;
    return voicelife::ParseJson(body, root).ok() && contracts::im::ParseReminderActionResult(root, validated).ok();
}

bool ValidatesAsActionStatusReport(const std::string& body) {
    voicelife::JsonValue root;
    contracts::im::ReminderActionStatusReport validated;
    return voicelife::ParseJson(body, root).ok() &&
           contracts::im::ParseReminderActionStatusReport(root, validated).ok();
}

}  // namespace

ImReportingChannel::ImReportingChannel(ImTransport& transport, ImCredentialProvider& credentials)
    : transport_(transport), credentials_(credentials) {}

ReportResult ImReportingChannel::SubmitScheduleReceipt(const contracts::im::ScheduleReceiptIntent& intent) {
    const std::string body = SerializeScheduleReceiptIntent(intent);
    if (!ValidatesAsScheduleReceipt(body)) {
        return {ReportStatus::kRejected, "发送前契约校验失败", ""};
    }
    return Submit(kScheduleReceiptPath, intent.eventId, intent.deviceId, body);
}

ReportResult ImReportingChannel::SubmitScheduleQueryResult(const contracts::im::ScheduleQueryResultIntent& intent) {
    const std::string body = SerializeScheduleQueryResultIntent(intent);
    if (!ValidatesAsScheduleQueryResult(body)) return {ReportStatus::kRejected, "发送前契约校验失败", ""};
    return Submit(kScheduleQueryResultPath, intent.businessEventId, intent.deviceId, body);
}

ReportResult ImReportingChannel::SubmitNotification(const contracts::im::NotificationIntent& intent) {
    const std::string body = SerializeNotificationIntent(intent);
    if (!ValidatesAsNotification(body)) {
        return {ReportStatus::kRejected, "发送前契约校验失败", ""};
    }
    return Submit(kNotificationPath, intent.businessEventId, intent.recipient.deviceId, body);
}

ReportResult ImReportingChannel::SubmitReminderActionResult(const contracts::im::ReminderActionResult& result,
                                                            const std::string& device_id,
                                                            const std::string& command_id) {
    const std::string body = SerializeReminderActionResult(result);
    if (!ValidatesAsActionResult(body)) {
        return {ReportStatus::kRejected, "发送前契约校验失败", ""};
    }
    const std::string path = kReminderActionResultPrefix + EncodePathSegment(device_id) + kReminderActionResultSuffix +
                             EncodePathSegment(command_id) + kReminderActionResultResultSuffix;
    return Submit(path, result.operationId, device_id, body);
}

ReportResult ImReportingChannel::SubmitReminderActionStatusReport(
    const contracts::im::ReminderActionStatusReport& report) {
    const std::string body = SerializeReminderActionStatusReport(report);
    if (!ValidatesAsActionStatusReport(body)) return {ReportStatus::kRejected, "发送前契约校验失败", ""};
    const std::string device_id = report.deviceId;
    const std::string path =
        kReminderActionResultPrefix + EncodePathSegment(device_id) + kReminderActionStatusPathSuffix;
    // MCP 结果与启动恢复扫描可能同时提交同一事实。设备端只抑制正文完全相同的
    // 已成功请求；不同正文仍交给 Gateway 做 eventId 冲突判定，失败请求也不缓存。
    std::lock_guard<std::mutex> lock(status_report_mutex_);
    const auto cached = submitted_status_report_bodies_.find(report.eventId);
    if (cached != submitted_status_report_bodies_.end() && cached->second == body) {
        return {ReportStatus::kSubmitted, "语音动作事实已在当前启动周期提交", ""};
    }
    const ReportResult result = Submit(path, report.eventId, device_id, body);
    if (result.status != ReportStatus::kSubmitted) return result;
    if (cached == submitted_status_report_bodies_.end()) {
        submitted_status_report_order_.push_back(report.eventId);
    }
    submitted_status_report_bodies_[report.eventId] = body;
    while (submitted_status_report_order_.size() > kSubmittedStatusReportCacheLimit) {
        const std::string evicted = std::move(submitted_status_report_order_.front());
        submitted_status_report_order_.pop_front();
        submitted_status_report_bodies_.erase(evicted);
    }
    return result;
}

ReportResult ImReportingChannel::Submit(const std::string& path, const std::string& idempotency_key,
                                        const std::string& intent_device_id, const std::string& body) {
    const std::string token = credentials_.DeviceToken();
    const std::string device_id = credentials_.DeviceId();
    if (token.empty()) {
        return {ReportStatus::kCredentialRejected, "设备凭据未配置", ""};
    }
    if (idempotency_key.empty()) {
        return {ReportStatus::kRejected, "幂等键不能为空", ""};
    }
    if (device_id.empty() || device_id != intent_device_id) {
        return {ReportStatus::kCredentialRejected, "deviceId 与意图不一致", ""};
    }

    ImHttpRequest request;
    request.path = path;
    request.method = "POST";
    request.body = body;
    request.headers = {{"Content-Type", "application/json"},
                       {"Authorization", "Bearer " + token},
                       {"Idempotency-Key", idempotency_key}};

    const ImHttpResponse response = transport_.Post(request);
    ReportResult result;
    switch (response.status) {
        case ImTransportStatus::kSuccess:
            result = {ReportStatus::kSubmitted, response.message, response.body};
            break;
        case ImTransportStatus::kCredentialRejected:
            result = {ReportStatus::kCredentialRejected, response.message, response.body};
            break;
        case ImTransportStatus::kNetworkFailure:
            result = {ReportStatus::kRetryable, response.message, response.body};
            break;
        case ImTransportStatus::kInvalidConfig:
            result = {ReportStatus::kRejected, response.message, response.body};
            break;
        case ImTransportStatus::kHttpError: {
            // 仅超时、限流与服务端 5xx 可重试；其余 4xx/3xx 为明确拒绝。
            const int code = response.status_code;
            if (code == 408 || code == 429 || code >= 500) {
                result = {ReportStatus::kRetryable, response.message, response.body};
            } else {
                result = {ReportStatus::kRejected, response.message, response.body};
            }
            break;
        }
    }
    return result;
}

}  // namespace voicelife::im
