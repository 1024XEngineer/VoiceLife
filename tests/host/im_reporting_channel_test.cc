// #126 设备侧 voicelife_im 上报通道：主机测试（TDD 先写）。
// 验收来源：Issue #126 —— 提交成功 / 凭据错误 / 网络失败三路径、
// 网络失败本地事实不变、提交意图使用事件 ID 幂等。
// 本文件先于实现存在，据此 pin 公共 API 形状与契约行为。

#include "voicelife/im/im_reporting_channel.h"

#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/test_support.h"
#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/reminder_action_status_report.h"
#include "voicelife/contracts/im/schedule_query_result.h"
#include "voicelife/contracts/im/schedule_receipt.h"
#include "voicelife/contracts/json.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_endpoint.h"
#include "voicelife/im/im_transport.h"

using voicelife::contracts::im::NotificationIntent;
using voicelife::contracts::im::ParseNotificationIntent;
using voicelife::contracts::im::ParseScheduleQueryResultIntent;
using voicelife::contracts::im::ParseScheduleReceiptIntent;
using voicelife::contracts::im::ReminderActionResult;
using voicelife::contracts::im::ReminderActionStatusReport;
using voicelife::contracts::im::ScheduleQueryResultIntent;
using voicelife::contracts::im::ScheduleReceiptIntent;
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

void TestIndependentActionStatusReport() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImReportingChannel channel(transport, credentials);
    ReminderActionStatusReport report;
    report.schemaVersion = "1";
    report.eventId = "voice-event-1";
    report.correlationId = "voice-correlation-1";
    report.deviceId = kDeviceId;
    report.reminderTriggerId = "trigger-fixture";
    report.operationId = "voice-operation-1";
    report.action = "snooze";
    report.status = "succeeded";
    report.occurredAt = "2026-08-03T00:01:00.000Z";
    report.nextTriggerAt = "2026-08-03T00:11:00.000Z";
    report.source = "voice";

    const ReportResult result = channel.SubmitReminderActionStatusReport(report);

    Check(result.status == ReportStatus::kSubmitted, "独立语音动作事实应提交成功");
    Check(transport.requests.size() == 1, "独立语音动作事实应发起一次传输");
    Check(transport.requests[0].path == "/v1/devices/device-fixture/reminder-action-status",
          "独立语音动作事实不得依赖 commandId 路径");
    Check(HeaderValue(transport.requests[0], "Idempotency-Key") == report.eventId,
          "独立语音动作事实幂等键必须使用 eventId");
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(transport.requests[0].body, root).ok(), "独立语音动作事实必须是合法 JSON");
    ReminderActionStatusReport parsed;
    Check(voicelife::contracts::im::ParseReminderActionStatusReport(root, parsed).ok(),
          "独立语音动作事实必须通过契约解析");
    Check(parsed.action == report.action && parsed.nextTriggerAt == report.nextTriggerAt && parsed.source == "voice",
          "独立语音动作事实字段必须完整保留");

    voicelife::JsonValue invalid_root;
    Check(voicelife::ParseJson("[]", invalid_root).ok(), "非法动作事实测试输入必须可解析");
    ReminderActionStatusReport invalid_parsed;
    Check(!voicelife::contracts::im::ParseReminderActionStatusReport(invalid_root, invalid_parsed).ok(),
          "非对象动作事实必须被契约解析拒绝");

    ReminderActionStatusReport invalid_ack = report;
    invalid_ack.action = "acknowledge";
    const ReportResult invalid_ack_result = channel.SubmitReminderActionStatusReport(invalid_ack);
    Check(invalid_ack_result.status == ReportStatus::kRejected, "成功确认动作携带 nextTriggerAt 必须在设备侧被拒绝");
    ReminderActionStatusReport invalid_snooze = report;
    invalid_snooze.nextTriggerAt.reset();
    const ReportResult invalid_snooze_result = channel.SubmitReminderActionStatusReport(invalid_snooze);
    Check(invalid_snooze_result.status == ReportStatus::kRejected, "成功延迟动作缺少 nextTriggerAt 必须在设备侧被拒绝");

    const ReportResult replay = channel.SubmitReminderActionStatusReport(report);
    Check(replay.status == ReportStatus::kSubmitted, "同一启动周期内相同语音事实重放应视为已提交");
    Check(transport.requests.size() == 1, "相同语音事实重放不得再次占用网络");

    report.nextTriggerAt = "2026-08-03T00:12:00.000Z";
    const ReportResult conflict = channel.SubmitReminderActionStatusReport(report);
    Check(conflict.status == ReportStatus::kSubmitted, "不同正文仍应交给 Gateway 处理，而不是被本地缓存吞掉");
    Check(transport.requests.size() == 2, "不同正文的同 eventId 必须再次发送以便 Gateway 判定冲突");

    transport.next_status = ImTransportStatus::kNetworkFailure;
    report.eventId = "voice-event-retry";
    const ReportResult failed = channel.SubmitReminderActionStatusReport(report);
    Check(failed.status == ReportStatus::kRetryable, "网络失败的语音事实必须保留重试机会");
    transport.next_status = ImTransportStatus::kSuccess;
    const ReportResult recovered = channel.SubmitReminderActionStatusReport(report);
    Check(recovered.status == ReportStatus::kSubmitted && transport.requests.size() == 4,
          "网络恢复后未成功提交的语音事实必须重新发送");
}

void TestIndependentActionStatusMalformedFields() {
    constexpr std::string_view kValid = R"({
        "schemaVersion":"1","eventId":"event","correlationId":"corr",
        "deviceId":"device","reminderTriggerId":"trigger","operationId":"operation",
        "action":"acknowledge","status":"succeeded","occurredAt":"2026-08-03T00:01:00.000Z",
        "source":"voice","details":{"reason":"child"}
    })";
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(kValid, root).ok(), "动作事实边界测试输入必须可解析");
    auto rejects = [&](const char* message) {
        ReminderActionStatusReport parsed;
        Check(!voicelife::contracts::im::ParseReminderActionStatusReport(root, parsed).ok(), message);
    };

    const std::array<const char*, 5> required = {"eventId", "correlationId", "deviceId", "reminderTriggerId",
                                                 "operationId"};
    for (const char* key : required) {
        const auto saved = root.object.at(key);
        root.object.erase(key);
        rejects("缺少动作事实必填字段必须拒绝");
        root.object.emplace(key, saved);
    }

    root.object["schemaVersion"] = voicelife::JsonValue::String("2");
    rejects("未知契约版本必须拒绝");
    root.object["schemaVersion"] = voicelife::JsonValue::String("1");
    root.object["eventId"] = voicelife::JsonValue::String("");
    rejects("空 eventId 必须拒绝");
    root.object["eventId"] = voicelife::JsonValue::String("event");
    root.object["action"] = voicelife::JsonValue::String("delete");
    rejects("未知动作类型必须拒绝");
    root.object["action"] = voicelife::JsonValue::String("acknowledge");
    root.object["status"] = voicelife::JsonValue::String("pending");
    rejects("未知动作状态必须拒绝");
    root.object["status"] = voicelife::JsonValue::String("succeeded");
    root.object["occurredAt"] = voicelife::JsonValue::String("2026-02-30T00:01:00Z");
    rejects("非法发生时间必须拒绝");
    root.object["occurredAt"] = voicelife::JsonValue::String("2026-08-03T00:01:00.000Z");
    root.object["nextTriggerAt"] = voicelife::JsonValue::String("not-a-time");
    rejects("非法 nextTriggerAt 必须拒绝");
    root.object.erase("nextTriggerAt");
    root.object["errorCode"] = voicelife::JsonValue::String("");
    rejects("空 errorCode 必须拒绝");
    root.object.erase("errorCode");
    root.object["details"] = voicelife::JsonValue::Array(std::vector<voicelife::JsonValue>(17));
    rejects("details 超出资源预算必须拒绝");
    root.object["details"] = voicelife::JsonValue::Object({});
    root.object["source"] = voicelife::JsonValue::String("im");
    rejects("非 voice 来源必须拒绝");
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
    TestIndependentActionStatusReport();
    TestIndependentActionStatusMalformedFields();
    TestGatewayUrlScheme();
    return 0;
}
