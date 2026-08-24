// #126 设备侧 voicelife_im 上报通道：主机测试（TDD 先写）。
// 验收来源：Issue #126 —— 提交成功 / 凭据错误 / 网络失败三路径、
// 网络失败本地事实不变、提交意图使用事件 ID 幂等。
// 本文件先于实现存在，据此 pin 公共 API 形状与契约行为。

#include "voicelife/im/im_reporting_channel.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "support/test_support.h"
#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/schedule_query_result.h"
#include "voicelife/contracts/im/schedule_receipt.h"
#include "voicelife/contracts/im/voice_reminder_action_status.h"
#include "voicelife/contracts/json.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_endpoint.h"
#include "voicelife/im/im_transport.h"

using voicelife::contracts::im::NotificationIntent;
using voicelife::contracts::im::ParseNotificationIntent;
using voicelife::contracts::im::ParseScheduleQueryResultIntent;
using voicelife::contracts::im::ParseScheduleReceiptIntent;
using voicelife::contracts::im::ParseVoiceReminderActionStatus;
using voicelife::contracts::im::ReminderActionResult;
using voicelife::contracts::im::ScheduleQueryResultIntent;
using voicelife::contracts::im::ScheduleReceiptIntent;
using voicelife::contracts::im::VoiceReminderActionStatus;
using voicelife::im::ImCredentialProvider;
using voicelife::im::ImHttpHeader;
using voicelife::im::ImHttpRequest;
using voicelife::im::ImHttpResponse;
using voicelife::im::ImReportingChannel;
using voicelife::im::ImTransport;
using voicelife::im::ImTransportStatus;
using voicelife::im::ReportResult;
using voicelife::im::ReportStatus;
using voicelife::test::Check;

namespace {

constexpr const char* kDeviceId = "device-fixture";
constexpr const char* kToken = "device-token";

std::string ReadFixture(const char* name) {
    std::ifstream input(std::string(VOICELIFE_SOURCE_DIR) + "/contracts/im-gateway/v1/fixtures/" + name);
    Check(input.good(), "共享 IM fixture 必须存在");
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

/// 记录请求并可控返回结果的假传输。
class FakeTransport : public ImTransport {
   public:
    std::vector<ImHttpRequest> requests;
    ImTransportStatus next_status = ImTransportStatus::kSuccess;
    int next_status_code = 200;
    std::string next_body;

    ImHttpResponse Post(const ImHttpRequest& request) override {
        requests.push_back(request);
        ImHttpResponse response;
        response.status = next_status;
        response.status_code = next_status_code;
        response.message = "fake";
        response.body = next_body;
        return response;
    }
    ImHttpResponse Get(const ImHttpRequest& request) override { return Post(request); }
};

/// 可控凭据的假凭据提供者。
class FakeCredentials : public ImCredentialProvider {
   public:
    std::string token = kToken;
    std::string device_id = kDeviceId;

    std::string DeviceToken() const override { return token; }
    std::string DeviceId() const override { return device_id; }
};

/// 从共享 fixture 构造契约意图，保证测试输入与双端契约一致。
ScheduleReceiptIntent MakeScheduleReceipt() {
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(ReadFixture("schedule-receipt.json"), root).ok(), "共享回执 fixture 必须可解析");
    ScheduleReceiptIntent intent;
    Check(ParseScheduleReceiptIntent(root, intent).ok(), "共享回执 fixture 必须通过契约校验");
    return intent;
}

NotificationIntent MakeNotification() {
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(ReadFixture("notification-strong.json"), root).ok(), "共享通知 fixture 必须可解析");
    NotificationIntent intent;
    Check(ParseNotificationIntent(root, intent).ok(), "共享通知 fixture 必须通过契约校验");
    return intent;
}

ScheduleQueryResultIntent MakeScheduleQueryResult() {
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(ReadFixture("schedule-query-result.json"), root).ok(),
          "共享查询结果 fixture 必须可解析");
    ScheduleQueryResultIntent intent;
    Check(ParseScheduleQueryResultIntent(root, intent).ok(), "共享查询结果 fixture 必须通过契约校验");
    return intent;
}

VoiceReminderActionStatus MakeVoiceStatus() {
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(ReadFixture("voice-reminder-action-status.json"), root).ok(),
          "共享语音状态 fixture 必须可解析");
    VoiceReminderActionStatus status;
    Check(ParseVoiceReminderActionStatus(root, status).ok(), "共享语音状态 fixture 必须通过契约校验");
    return status;
}

std::string HeaderValue(const ImHttpRequest& request, const std::string& name) {
    for (const ImHttpHeader& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return "";
}

/// 校验提交的请求体可通过契约解析，且与提交的意图逐字段一致。
void CheckBodyRoundTrips(const ImHttpRequest& request, const ScheduleReceiptIntent& intent) {
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(request.body, root).ok(), "回执请求体必须是合法 JSON");
    ScheduleReceiptIntent parsed;
    Check(ParseScheduleReceiptIntent(root, parsed).ok(), "回执请求体必须通过契约校验");
    Check(parsed.schemaVersion == intent.schemaVersion && parsed.eventId == intent.eventId &&
              parsed.correlationId == intent.correlationId && parsed.userId == intent.userId &&
              parsed.deviceId == intent.deviceId && parsed.operationType == intent.operationType &&
              parsed.scheduleId == intent.scheduleId && parsed.result == intent.result &&
              parsed.summary == intent.summary && parsed.occurredAt == intent.occurredAt,
          "回执请求体必须与提交的意图完全一致");
}

void CheckBodyRoundTrips(const ImHttpRequest& request, const NotificationIntent& intent) {
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(request.body, root).ok(), "通知请求体必须是合法 JSON");
    NotificationIntent parsed;
    Check(ParseNotificationIntent(root, parsed).ok(), "通知请求体必须通过契约校验");
    Check(parsed.schemaVersion == intent.schemaVersion && parsed.businessEventId == intent.businessEventId &&
              parsed.correlationId == intent.correlationId && parsed.kind == intent.kind &&
              parsed.recipient.userId == intent.recipient.userId &&
              parsed.recipient.deviceId == intent.recipient.deviceId && parsed.scheduleId == intent.scheduleId &&
              parsed.taskId == intent.taskId && parsed.instanceId == intent.instanceId &&
              parsed.reminderTriggerId == intent.reminderTriggerId && parsed.reminderType == intent.reminderType &&
              parsed.content.title == intent.content.title && parsed.content.body == intent.content.body &&
              parsed.plannedAt == intent.plannedAt && parsed.triggerAt == intent.triggerAt &&
              parsed.occurredAt == intent.occurredAt && parsed.actions.size() == intent.actions.size(),
          "通知请求体必须与提交的意图完全一致");
    for (size_t i = 0; i < intent.actions.size(); ++i) {
        Check(parsed.actions[i].kind == intent.actions[i].kind && parsed.actions[i].type == intent.actions[i].type &&
                  parsed.actions[i].label == intent.actions[i].label &&
                  parsed.actions[i].minutes == intent.actions[i].minutes,
              "通知动作必须与提交的意图一致");
    }
}

void CheckBodyRoundTrips(const ImHttpRequest& request, const VoiceReminderActionStatus& intent) {
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(request.body, root).ok(), "语音状态请求体必须是合法 JSON");
    VoiceReminderActionStatus parsed;
    Check(ParseVoiceReminderActionStatus(root, parsed).ok(), "语音状态请求体必须通过契约校验");
    Check(parsed.schemaVersion == intent.schemaVersion && parsed.eventId == intent.eventId &&
              parsed.correlationId == intent.correlationId && parsed.deviceId == intent.deviceId &&
              parsed.reminderTriggerId == intent.reminderTriggerId && parsed.operationId == intent.operationId &&
              parsed.action == intent.action && parsed.status == intent.status &&
              parsed.occurredAt == intent.occurredAt && parsed.nextTriggerAt == intent.nextTriggerAt &&
              parsed.source == intent.source,
          "语音状态请求体必须与提交事实完全一致");
}

void TestScheduleReceiptSuccess() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImReportingChannel channel(transport, credentials);
    const ScheduleReceiptIntent intent = MakeScheduleReceipt();

    const ReportResult result = channel.SubmitScheduleReceipt(intent);

    Check(result.status == ReportStatus::kSubmitted, "日程回执提交成功");
    Check(transport.requests.size() == 1, "日程回执应发起一次传输");
    const ImHttpRequest& request = transport.requests[0];
    Check(request.path == "/v1/im/schedule-receipts", "日程回执必须提交到 schedule-receipts 路径");
    Check(request.method == "POST", "提交必须使用 POST");
    Check(HeaderValue(request, "Content-Type") == "application/json", "必须声明 JSON 请求体");
    Check(HeaderValue(request, "Authorization") == "Bearer " + std::string(kToken), "必须携带设备令牌");
    Check(HeaderValue(request, "Idempotency-Key") == intent.eventId, "幂等键必须等于回执事件 ID");
    CheckBodyRoundTrips(request, intent);
}

void TestNotificationSuccess() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImReportingChannel channel(transport, credentials);
    const NotificationIntent intent = MakeNotification();

    const ReportResult result = channel.SubmitNotification(intent);

    Check(result.status == ReportStatus::kSubmitted, "通知提交成功");
    Check(transport.requests.size() == 1, "通知应发起一次传输");
    const ImHttpRequest& request = transport.requests[0];
    Check(request.path == "/v1/im/notifications", "通知必须提交到统一的 notifications 路径");
    Check(request.method == "POST", "提交必须使用 POST");
    Check(HeaderValue(request, "Authorization") == "Bearer " + std::string(kToken), "必须携带设备令牌");
    Check(HeaderValue(request, "Idempotency-Key") == intent.businessEventId, "幂等键必须等于业务事件 ID");
    Check(request.path.find("/v1/notification-intents") == std::string::npos,
          "不得再使用旧的 notification-intents 路径");
    CheckBodyRoundTrips(request, intent);
}

void TestScheduleQueryResultSuccess() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImReportingChannel channel(transport, credentials);
    const ScheduleQueryResultIntent intent = MakeScheduleQueryResult();

    const ReportResult result = channel.SubmitScheduleQueryResult(intent);

    Check(result.status == ReportStatus::kSubmitted, "完整日程查询结果提交成功");
    Check(transport.requests.size() == 1, "完整日程查询结果应发起一次传输");
    const ImHttpRequest& request = transport.requests[0];
    Check(request.path == "/v1/im/schedule-query-results", "查询结果必须提交到专用 Gateway 路径");
    Check(HeaderValue(request, "Idempotency-Key") == intent.businessEventId, "查询结果幂等键必须使用业务事件 ID");
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(request.body, root).ok(), "查询结果请求体必须是合法 JSON");
    ScheduleQueryResultIntent parsed;
    Check(ParseScheduleQueryResultIntent(root, parsed).ok(), "查询结果请求体必须通过契约校验");
    Check(parsed.resultCount == intent.resultCount && parsed.schedules.array.size() == 1 &&
              parsed.futureOccurrences.array.size() == 1 && parsed.exceptions.array.size() == 1,
          "查询结果请求体必须保留完整条目集合");
}

void TestVoiceReminderActionStatusSuccess() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImReportingChannel channel(transport, credentials);
    const VoiceReminderActionStatus status = MakeVoiceStatus();

    const ReportResult result = channel.SubmitVoiceReminderActionStatus(status);

    Check(result.status == ReportStatus::kSubmitted, "语音状态提交成功");
    Check(transport.requests.size() == 1, "语音状态应发起一次传输");
    const ImHttpRequest& request = transport.requests[0];
    Check(request.path == "/v1/im/reminder-action-statuses", "语音状态必须提交到专用 Gateway 路径");
    Check(request.method == "POST", "语音状态提交必须使用 POST");
    Check(HeaderValue(request, "Authorization") == "Bearer " + std::string(kToken), "语音状态必须携带设备令牌");
    Check(HeaderValue(request, "Idempotency-Key") == status.eventId, "语音状态幂等键必须使用 eventId");
    CheckBodyRoundTrips(request, status);
}

void TestVoiceReminderActionStatusInvalidLocally() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImReportingChannel channel(transport, credentials);
    VoiceReminderActionStatus status = MakeVoiceStatus();
    status.source = "h5";

    const ReportResult result = channel.SubmitVoiceReminderActionStatus(status);

    Check(result.status == ReportStatus::kRejected, "非法语音来源必须在发送前拒绝");
    Check(transport.requests.empty(), "非法语音状态不得发起网络请求");
}

void TestScheduleQueryResultNetworkFailureIsRetryable() {
    FakeTransport transport;
    FakeCredentials credentials;
    transport.next_status = ImTransportStatus::kNetworkFailure;
    ImReportingChannel channel(transport, credentials);

    const ReportResult result = channel.SubmitScheduleQueryResult(MakeScheduleQueryResult());

    Check(result.status == ReportStatus::kRetryable, "查询结果网络失败必须保留可重试分类");
    Check(transport.requests.size() == 1, "查询结果网络失败仍应记录一次发送尝试");
}

void TestScheduleQueryResultOptionalFieldsRoundTrip() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImReportingChannel channel(transport, credentials);
    ScheduleQueryResultIntent intent = MakeScheduleQueryResult();
    intent.userId.reset();
    intent.keyword = "会议";
    intent.startDate.reset();
    intent.endDate.reset();

    const ReportResult result = channel.SubmitScheduleQueryResult(intent);

    Check(result.status == ReportStatus::kSubmitted, "可选查询字段的组合必须可提交");
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(transport.requests[0].body, root).ok(), "可选查询字段请求体必须是合法 JSON");
    ScheduleQueryResultIntent parsed;
    Check(ParseScheduleQueryResultIntent(root, parsed).ok(), "可选查询字段请求体必须通过契约校验");
    Check(!parsed.userId.has_value() && parsed.keyword == "会议" && !parsed.startDate.has_value() &&
              !parsed.endDate.has_value(),
          "序列化必须精确保留查询结果可选字段是否存在");
}

void TestScheduleQueryResultInvalidIntentRejectedLocally() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImReportingChannel channel(transport, credentials);
    ScheduleQueryResultIntent intent = MakeScheduleQueryResult();
    intent.queriedAt = "invalid";

    const ReportResult result = channel.SubmitScheduleQueryResult(intent);

    Check(result.status == ReportStatus::kRejected, "非法查询结果必须在发送前本地拒绝");
    Check(transport.requests.empty(), "发送前契约校验失败不得发起网络请求");
}

void TestSubmitNotificationSurfacesResponseBody() {
    FakeTransport transport;
    FakeCredentials credentials;
    const std::string submission = ReadFixture("notification-submission.json");
    transport.next_body = submission;
    ImReportingChannel channel(transport, credentials);

    const ReportResult result = channel.SubmitNotification(MakeNotification());

    Check(result.status == ReportStatus::kSubmitted, "受理成功状态必须保留");
    Check(result.response_body == submission, "网关受理结果响应体必须原样透传");
}

void TestMissingCredentialIsLocal() {
    FakeTransport transport;
    FakeCredentials credentials;
    credentials.token = "";
    ImReportingChannel channel(transport, credentials);

    const ReportResult result = channel.SubmitNotification(MakeNotification());

    Check(result.status == ReportStatus::kCredentialRejected, "空令牌必须本地拒绝");
    Check(transport.requests.empty(), "凭据错误不得发起网络请求");
}

void TestDeviceIdMismatchIsLocal() {
    FakeTransport transport;
    FakeCredentials credentials;
    credentials.device_id = "other-device";
    ImReportingChannel channel(transport, credentials);

    const ReportResult result = channel.SubmitScheduleReceipt(MakeScheduleReceipt());

    Check(result.status == ReportStatus::kCredentialRejected, "deviceId 不一致必须本地拒绝");
    Check(transport.requests.empty(), "deviceId 不一致不得发起网络请求");
}

void TestCredentialRejectedByServer() {
    FakeTransport transport;
    FakeCredentials credentials;
    transport.next_status = ImTransportStatus::kCredentialRejected;
    transport.next_status_code = 401;
    ImReportingChannel channel(transport, credentials);

    const ReportResult result = channel.SubmitScheduleReceipt(MakeScheduleReceipt());

    Check(result.status == ReportStatus::kCredentialRejected, "401 必须归类为凭据错误");
    Check(transport.requests.size() == 1, "服务端拒绝仍应发起一次传输");
}

void TestNetworkFailureKeepsFactsAndAllowsIdempotentRetry() {
    FakeTransport transport;
    FakeCredentials credentials;
    transport.next_status = ImTransportStatus::kNetworkFailure;
    ImReportingChannel channel(transport, credentials);
    const NotificationIntent original = MakeNotification();

    const ReportResult first = channel.SubmitNotification(original);
    Check(first.status == ReportStatus::kRetryable, "网络失败必须归类为可重试");
    Check(transport.requests.size() == 1, "网络失败应发起一次传输");

    const ReportResult retry = channel.SubmitNotification(original);
    Check(retry.status == ReportStatus::kRetryable, "重试后网络仍失败保持可重试");
    Check(transport.requests.size() == 2, "相同事件 ID 允许重试");
    Check(HeaderValue(transport.requests[1], "Idempotency-Key") == original.businessEventId, "重试必须复用相同幂等键");
    Check(transport.requests[1].body == transport.requests[0].body, "重试必须携带相同请求体");
    Check(original.businessEventId == "event-fixture" && original.recipient.deviceId == kDeviceId &&
              !original.actions.empty(),
          "提交不得修改本地事实");
}

void TestInvalidIntentRejectedLocally() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImReportingChannel channel(transport, credentials);

    ScheduleReceiptIntent no_event = MakeScheduleReceipt();
    no_event.eventId = "";
    Check(channel.SubmitScheduleReceipt(no_event).status == ReportStatus::kRejected, "空回执事件 ID 必须本地拒绝");
    Check(transport.requests.empty(), "空回执事件 ID 不得发起网络请求");

    NotificationIntent no_business_event = MakeNotification();
    no_business_event.businessEventId = "";
    Check(channel.SubmitNotification(no_business_event).status == ReportStatus::kRejected,
          "空业务事件 ID 必须本地拒绝");
    Check(transport.requests.empty(), "空业务事件 ID 不得发起网络请求");

    NotificationIntent bad_type = MakeNotification();
    bad_type.reminderType = "urgent";
    Check(channel.SubmitNotification(bad_type).status == ReportStatus::kRejected, "非法提醒类型必须本地拒绝");
    Check(transport.requests.empty(), "非法提醒类型不得发起网络请求");

    NotificationIntent snooze_without_minutes = MakeNotification();
    snooze_without_minutes.actions[1].minutes.reset();
    Check(channel.SubmitNotification(snooze_without_minutes).status == ReportStatus::kRejected,
          "snooze 缺 minutes 必须本地拒绝");
    Check(transport.requests.empty(), "snooze 缺 minutes 不得发起网络请求");
}

void TestStatusCodeMapping() {
    struct Case {
        int code;
        ReportStatus expected;
        const char* why;
    };
    const Case cases[] = {
        {400, ReportStatus::kRejected, "400 客户端错误不可重试"},  {409, ReportStatus::kRejected, "409 冲突不可重试"},
        {422, ReportStatus::kRejected, "422 语义错误不可重试"},    {301, ReportStatus::kRejected, "重定向不可重试"},
        {408, ReportStatus::kRetryable, "408 超时可重试"},         {429, ReportStatus::kRetryable, "429 限流可重试"},
        {503, ReportStatus::kRetryable, "503 服务暂不可用可重试"},
    };
    for (const Case& c : cases) {
        FakeTransport transport;
        FakeCredentials credentials;
        transport.next_status = ImTransportStatus::kHttpError;
        transport.next_status_code = c.code;
        ImReportingChannel channel(transport, credentials);
        const ReportResult result = channel.SubmitScheduleReceipt(MakeScheduleReceipt());
        Check(result.status == c.expected, c.why);
    }
}

void TestInvalidTransportConfigIsRejected() {
    FakeTransport transport;
    FakeCredentials credentials;
    transport.next_status = ImTransportStatus::kInvalidConfig;
    ImReportingChannel channel(transport, credentials);

    const ReportResult result = channel.SubmitScheduleReceipt(MakeScheduleReceipt());

    Check(result.status == ReportStatus::kRejected, "传输配置错误必须归类为拒绝");
    Check(transport.requests.size() == 1, "传输配置错误仍应被通道映射");
}

void TestActionResultPathEncodesSegments() {
    FakeTransport transport;
    FakeCredentials credentials;
    credentials.device_id = "dev/ice?x=1";
    ImReportingChannel channel(transport, credentials);

    ReminderActionResult result;
    result.schemaVersion = "1";
    result.operationId = "operation-1";
    result.reminderTriggerId = "trigger-fixture";
    result.status = "succeeded";
    result.occurredAt = "2026-08-03T00:01:00.000Z";
    const ReportResult report = channel.SubmitReminderActionResult(result, credentials.device_id, "cmd/1#x");

    Check(report.status == ReportStatus::kSubmitted, "编码路径段后提交应成功");
    Check(transport.requests.size() == 1, "编码路径段后应发起一次传输");
    Check(transport.requests[0].path == "/v1/devices/dev%2Fice%3Fx%3D1/reminder-actions/cmd%2F1%23x/result",
          "deviceId 与 commandId 必须按 path 段百分号编码，不得改写路径");
}

void TestGatewayUrlScheme() {
    Check(voicelife::im::IsHttpsGatewayUrl("https://im.example.com"), "https 基地址必须通过");
    Check(!voicelife::im::IsHttpsGatewayUrl("http://im.example.com"), "http 基地址必须拒绝");
    Check(!voicelife::im::IsHttpsGatewayUrl("im.example.com"), "缺失 scheme 必须拒绝");
    Check(!voicelife::im::IsHttpsGatewayUrl(""), "空基地址必须拒绝");
    Check(!voicelife::im::IsHttpsGatewayUrl("https://im.example.com?x=1"), "带 query 必须拒绝");
    Check(!voicelife::im::IsHttpsGatewayUrl("https://im.example.com#frag"), "带 fragment 必须拒绝");
}

}  // namespace

int main() {
    TestScheduleReceiptSuccess();
    TestNotificationSuccess();
    TestScheduleQueryResultSuccess();
    TestVoiceReminderActionStatusSuccess();
    TestVoiceReminderActionStatusInvalidLocally();
    TestScheduleQueryResultNetworkFailureIsRetryable();
    TestScheduleQueryResultOptionalFieldsRoundTrip();
    TestScheduleQueryResultInvalidIntentRejectedLocally();
    TestSubmitNotificationSurfacesResponseBody();
    TestMissingCredentialIsLocal();
    TestDeviceIdMismatchIsLocal();
    TestCredentialRejectedByServer();
    TestNetworkFailureKeepsFactsAndAllowsIdempotentRetry();
    TestInvalidIntentRejectedLocally();
    TestStatusCodeMapping();
    TestInvalidTransportConfigIsRejected();
    TestActionResultPathEncodesSegments();
    TestGatewayUrlScheme();
    return 0;
}
