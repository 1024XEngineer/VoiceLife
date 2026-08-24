#include <fstream>
#include <sstream>
#include <string>

#include "support/test_support.h"
#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/im/notification_submission.h"
#include "voicelife/contracts/im/pairing_session.h"
#include "voicelife/contracts/im/reminder_action_command.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/schedule_query_result.h"
#include "voicelife/contracts/im/schedule_receipt.h"
#include "voicelife/contracts/im/voice_reminder_action_status.h"
#include "voicelife/contracts/json.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::Status;
using voicelife::contracts::im::CreatedPairingSession;
using voicelife::contracts::im::CreatePairingSessionRequest;
using voicelife::contracts::im::kDeviceContractVersion;
using voicelife::contracts::im::NotificationIntent;
using voicelife::contracts::im::NotificationSubmission;
using voicelife::contracts::im::PairingSessionStatus;
using voicelife::contracts::im::ParseCreatedPairingSession;
using voicelife::contracts::im::ParseCreatePairingSessionRequest;
using voicelife::contracts::im::ParseNotificationIntent;
using voicelife::contracts::im::ParseNotificationSubmission;
using voicelife::contracts::im::ParsePairingSessionStatus;
using voicelife::contracts::im::ParseReminderActionCommand;
using voicelife::contracts::im::ParseReminderActionResult;
using voicelife::contracts::im::ParseScheduleQueryResultIntent;
using voicelife::contracts::im::ParseScheduleReceiptIntent;
using voicelife::contracts::im::ParseVoiceReminderActionStatus;
using voicelife::contracts::im::ReminderActionCommand;
using voicelife::contracts::im::ReminderActionResult;
using voicelife::contracts::im::ScheduleQueryResultIntent;
using voicelife::contracts::im::ScheduleReceiptIntent;
using voicelife::contracts::im::VoiceReminderActionStatus;
using voicelife::test::Check;

namespace {

std::string ReadFixture(const char* name) {
    std::ifstream input(std::string(VOICELIFE_SOURCE_DIR) + "/contracts/im-gateway/v1/fixtures/" + name);
    Check(input.good(), "共享 IM fixture 必须存在");
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

Status ParseFixture(const char* name, NotificationIntent& out) {
    JsonValue root;
    if (Status json_status = voicelife::ParseJson(ReadFixture(name), root); !json_status.ok()) {
        return json_status;
    }
    return ParseNotificationIntent(root, out);
}

template <typename Output, typename Parser>
Status ParsePairingFixture(const char* name, Output& out, Parser parser) {
    JsonValue root;
    if (Status json_status = voicelife::ParseJson(ReadFixture(name), root); !json_status.ok()) {
        return json_status;
    }
    return parser(root, out);
}

Status ParseScheduleReceiptFixture(const char* name, ScheduleReceiptIntent& out) {
    JsonValue root;
    if (Status json_status = voicelife::ParseJson(ReadFixture(name), root); !json_status.ok()) {
        return json_status;
    }
    return ParseScheduleReceiptIntent(root, out);
}

Status ParseScheduleQueryResultFixture(const char* name, ScheduleQueryResultIntent& out) {
    JsonValue root;
    if (Status json_status = voicelife::ParseJson(ReadFixture(name), root); !json_status.ok()) return json_status;
    return ParseScheduleQueryResultIntent(root, out);
}

Status ParseActionResultFixture(const char* name, ReminderActionResult& out) {
    JsonValue root;
    if (Status json_status = voicelife::ParseJson(ReadFixture(name), root); !json_status.ok()) {
        return json_status;
    }
    return ParseReminderActionResult(root, out);
}

Status ParseVoiceStatusFixture(const char* name, VoiceReminderActionStatus& out) {
    JsonValue root;
    if (Status json_status = voicelife::ParseJson(ReadFixture(name), root); !json_status.ok()) {
        return json_status;
    }
    return ParseVoiceReminderActionStatus(root, out);
}

void RequireRejected(const char* name, const char* message) {
    NotificationIntent intent;
    const Status status = ParseFixture(name, intent);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, message);
}

void RequireScheduleReceiptRejected(const char* name, const char* message) {
    ScheduleReceiptIntent intent;
    const Status status = ParseScheduleReceiptFixture(name, intent);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, message);
}

void RequireScheduleQueryResultRejected(const JsonValue& root, const char* message) {
    ScheduleQueryResultIntent intent;
    const Status status = ParseScheduleQueryResultIntent(root, intent);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, message);
}

void RequireActionResultRejected(const char* name, const char* message) {
    ReminderActionResult intent;
    const Status status = ParseActionResultFixture(name, intent);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, message);
}

void RequireVoiceStatusRejected(const char* name, const char* message) {
    VoiceReminderActionStatus status;
    const Status parsed = ParseVoiceStatusFixture(name, status);
    Check(!parsed.ok() && parsed.code == ErrorCode::kInvalidArgument, message);
}

Status ParseCommandFixture(const char* name, ReminderActionCommand& out) {
    JsonValue root;
    if (Status json_status = voicelife::ParseJson(ReadFixture(name), root); !json_status.ok()) {
        return json_status;
    }
    return ParseReminderActionCommand(root, out);
}

void RequireCommandRejected(const char* name, const char* message) {
    ReminderActionCommand command;
    const Status status = ParseCommandFixture(name, command);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, message);
}

Status ParseSubmissionFixture(const char* name, NotificationSubmission& out) {
    JsonValue root;
    if (Status json_status = voicelife::ParseJson(ReadFixture(name), root); !json_status.ok()) {
        return json_status;
    }
    return ParseNotificationSubmission(root, out);
}

void RequireSubmissionRejected(const char* name, const char* message) {
    NotificationSubmission submission;
    const Status status = ParseSubmissionFixture(name, submission);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, message);
}

}  // namespace

int main() {
    CreatePairingSessionRequest pairing_request;
    Check(ParsePairingFixture("pairing-create-request.json", pairing_request, ParseCreatePairingSessionRequest).ok(),
          "完整配对创建请求 fixture 必须被 C++ 解析");
    Check(pairing_request.deviceId == "device-fixture" && pairing_request.userId == "user-fixture" &&
              pairing_request.allowedPlatforms.has_value() && pairing_request.allowedPlatforms->size() == 1 &&
              pairing_request.expiresInMinutes == 5,
          "配对创建请求字段必须与 TypeScript 语义一致");
    CreatePairingSessionRequest minimal_pairing_request;
    Check(ParsePairingFixture("pairing-create-request-minimal.json", minimal_pairing_request,
                              ParseCreatePairingSessionRequest)
              .ok(),
          "最小配对创建请求 fixture 必须被 C++ 解析");
    Check(!ParsePairingFixture("pairing-create-request-invalid-expiry.json", pairing_request,
                               ParseCreatePairingSessionRequest)
               .ok(),
          "越界配对有效期必须被 C++ 拒绝");
    Check(!ParsePairingFixture("pairing-create-request-invalid-platform.json", pairing_request,
                               ParseCreatePairingSessionRequest)
               .ok(),
          "未知配对平台必须被 C++ 拒绝");
    for (const char* invalid_request : {R"([])", R"({"deviceId":"device-fixture","allowedPlatforms":{}})",
                                        R"({"deviceId":"device-fixture","expiresInMinutes":1.5})"}) {
        JsonValue root;
        Check(ParseJson(invalid_request, root).ok() && !ParseCreatePairingSessionRequest(root, pairing_request).ok(),
              "配对请求的对象、平台数组和整数有效期约束必须执行");
    }
    JsonValue all_platforms;
    Check(
        ParseJson(
            R"({"deviceId":"device-fixture","allowedPlatforms":["wechat_official","wecom_aibot","feishu","dingtalk"]})",
            all_platforms)
                .ok() &&
            ParseCreatePairingSessionRequest(all_platforms, pairing_request).ok(),
        "全部声明平台必须被契约解析器识别");
    JsonValue invalid_empty_platforms;
    Check(ParseJson(R"({"deviceId":"device-fixture","allowedPlatforms":[]})", invalid_empty_platforms).ok() &&
              !ParseCreatePairingSessionRequest(invalid_empty_platforms, pairing_request).ok(),
          "空平台集合必须被 C++ 拒绝");
    JsonValue invalid_duplicate_platforms;
    Check(ParseJson(R"({"deviceId":"device-fixture","allowedPlatforms":["wechat_official","wechat_official"]})",
                    invalid_duplicate_platforms)
                  .ok() &&
              !ParseCreatePairingSessionRequest(invalid_duplicate_platforms, pairing_request).ok(),
          "重复平台必须被 C++ 拒绝");

    CreatedPairingSession created_pairing;
    Check(ParsePairingFixture("pairing-created.json", created_pairing, ParseCreatedPairingSession).ok(),
          "配对创建响应 fixture 必须被 C++ 解析");
    Check(created_pairing.displayCode == "123456" && created_pairing.session.status == "pending",
          "创建响应必须保留六位展示码和 pending 状态");
    Check(!ParsePairingFixture("pairing-created-invalid-code.json", created_pairing, ParseCreatedPairingSession).ok(),
          "非六位数字 displayCode 必须被 C++ 拒绝");
    Check(!ParsePairingFixture("pairing-created-invalid-secret.json", created_pairing, ParseCreatedPairingSession).ok(),
          "含 displayCodeHash 的创建响应必须 fail closed");
    for (
        const char* invalid_created :
        {R"([])", R"({"displayCode":"123456"})",
         R"({"session":{"id":"pairing-1","deviceId":"device-fixture","status":"expired","expiresAt":"2026-08-03T00:05:00Z","createdAt":"2026-08-03T00:00:00Z"},"displayCode":"123456"})"}) {
        JsonValue root;
        Check(ParseJson(invalid_created, root).ok() && !ParseCreatedPairingSession(root, created_pairing).ok(),
              "创建响应必须是含 pending session 的对象");
    }

    PairingSessionStatus pairing_status;
    Check(ParsePairingFixture("pairing-status.json", pairing_status, ParsePairingSessionStatus).ok(),
          "pending 配对状态 fixture 必须被 C++ 解析");
    Check(ParsePairingFixture("pairing-status-confirmed.json", pairing_status, ParsePairingSessionStatus).ok() &&
              pairing_status.status == "confirmed" && pairing_status.confirmedAt.has_value(),
          "confirmed 配对状态必须保留 confirmedAt");
    Check(ParsePairingFixture("pairing-status-expired.json", pairing_status, ParsePairingSessionStatus).ok() &&
              pairing_status.status == "expired",
          "expired 配对状态必须被 C++ 解析");
    Check(ParsePairingFixture("pairing-status-cancelled.json", pairing_status, ParsePairingSessionStatus).ok() &&
              pairing_status.status == "cancelled",
          "cancelled 配对状态必须被 C++ 解析");
    Check(!ParsePairingFixture("pairing-status-invalid-status.json", pairing_status, ParsePairingSessionStatus).ok(),
          "未知配对状态必须被 C++ 拒绝");
    Check(!ParsePairingFixture("pairing-status-invalid-time.json", pairing_status, ParsePairingSessionStatus).ok(),
          "非法配对时间必须被 C++ 拒绝");
    Check(!ParsePairingFixture("pairing-status-invalid-secret.json", pairing_status, ParsePairingSessionStatus).ok(),
          "含 displayCodeHash 的状态响应必须 fail closed");
    for (
        const char* invalid_status :
        {R"([])",
         R"({"id":"pairing-1","deviceId":"device-fixture","status":"confirmed","createdAt":"2026-08-03T00:00:00Z","expiresAt":"2026-08-03T00:05:00Z","confirmedAt":"2026-08-02T23:59:59Z"})",
         R"({"id":"pairing-1","deviceId":"device-fixture","status":"confirmed","createdAt":"2026-08-03T00:00:00Z","expiresAt":"2026-08-03T00:05:00Z","confirmedAt":"2026-08-03T00:05:00Z"})"}) {
        JsonValue root;
        Check(ParseJson(invalid_status, root).ok() && !ParsePairingSessionStatus(root, pairing_status).ok(),
              "状态对象及 confirmedAt 有效窗口约束必须执行");
    }
    JsonValue invalid_pairing_time_order;
    Check(
        ParseJson(
            R"({"id":"pairing-1","deviceId":"device-fixture","status":"pending","createdAt":"2026-08-03T00:05:00.000Z","expiresAt":"2026-08-03T00:00:00.000Z"})",
            invalid_pairing_time_order)
                .ok() &&
            !ParsePairingSessionStatus(invalid_pairing_time_order, pairing_status).ok(),
        "expiresAt 不晚于 createdAt 的响应必须 fail closed");
    JsonValue invalid_nested_secret;
    Check(
        ParseJson(
            R"({"id":"pairing-1","deviceId":"device-fixture","status":"pending","createdAt":"2026-08-03T00:00:00.000Z","expiresAt":"2026-08-03T00:05:00.000Z","extension":{"externalUserId":"secret"}})",
            invalid_nested_secret)
                .ok() &&
            !ParsePairingSessionStatus(invalid_nested_secret, pairing_status).ok(),
        "嵌套外部身份字段也必须 fail closed");
    JsonValue invalid_top_level_secret;
    Check(
        ParseJson(
            R"({"session":{"id":"pairing-1","deviceId":"device-fixture","status":"pending","createdAt":"2026-08-03T00:00:00.000Z","expiresAt":"2026-08-03T00:05:00.000Z"},"displayCode":"123456","displayCodeHash":"secret"})",
            invalid_top_level_secret)
                .ok() &&
            !ParseCreatedPairingSession(invalid_top_level_secret, created_pairing).ok(),
        "创建响应顶层额外字段必须 fail closed");

    // 强提醒：双端共享版本，字段与 TS 语义一致
    NotificationIntent strong;
    Check(ParseFixture("notification-strong.json", strong).ok(), "共享强提醒 fixture 必须被 C++ 解析");
    Check(strong.schemaVersion == kDeviceContractVersion, "C++ 与 TypeScript 必须共享设备契约版本");
    Check(strong.reminderType == "strong", "强提醒 fixture 的 reminderType 必须为 strong");
    Check(strong.actions.size() == 2 && strong.actions[0].type == "acknowledge" && strong.actions[1].type == "snooze" &&
              strong.actions[1].minutes == 10,
          "强提醒动作必须与 TS 语义一致");
    Check(strong.actions[0].label == "知道了" && strong.actions[1].label == "推迟 10 分钟",
          "UTF-8 动作标签必须被原样保留");
    Check(strong.content.title == "Fixture reminder", "通知内容标题必须被保留");
    Check(strong.recipient.deviceId == "device-fixture" && strong.recipient.userId == "user-fixture",
          "收件人字段必须被保留");
    Check(strong.reminderTriggerId == "trigger-fixture", "reminderTriggerId 必须被保留");

    // 弱提醒：不得携带动作
    NotificationIntent weak;
    Check(ParseFixture("notification-weak.json", weak).ok(), "共享弱提醒 fixture 必须被 C++ 解析");
    Check(weak.reminderType == "weak" && weak.actions.empty(), "弱提醒 fixture 不得携带动作");

    // 非法 fixture：与 TS 一致的拒绝语义
    RequireRejected("notification-invalid-version.json", "非法版本 fixture 必须被 C++ 拒绝");
    RequireRejected("notification-invalid-enum.json", "非法枚举 fixture 必须被 C++ 拒绝");
    RequireRejected("notification-invalid-time.json", "非法时间 fixture 必须被 C++ 拒绝");
    RequireRejected("notification-missing-field.json", "缺字段 fixture 必须被 C++ 拒绝");

    // 日程回执：字段与 TS parseScheduleReceiptIntent 一致
    ScheduleReceiptIntent receipt;
    Check(ParseScheduleReceiptFixture("schedule-receipt.json", receipt).ok(), "共享日程回执 fixture 必须被 C++ 解析");
    Check(receipt.schemaVersion == kDeviceContractVersion, "日程回执必须共享设备契约版本");
    Check(receipt.eventId == "event-schedule-fixture" && receipt.correlationId == "correlation-schedule-fixture",
          "日程回执事件与关联标识必须被保留");
    Check(receipt.userId.has_value() && *receipt.userId == "user-fixture", "可选 userId 必须被保留");
    Check(receipt.deviceId == "device-fixture", "日程回执 deviceId 必须被保留");
    Check(receipt.operationType == "created" && receipt.result == "succeeded", "日程回执枚举必须与 TS 语义一致");
    Check(receipt.scheduleId == "schedule-fixture" && receipt.summary == "日程已创建", "日程回执载荷必须被保留");

    ScheduleQueryResultIntent query_result;
    Check(ParseScheduleQueryResultFixture("schedule-query-result.json", query_result).ok(),
          "完整日程查询结果 fixture 必须被 C++ 解析");
    Check(query_result.resultCount == 2 && query_result.schedules.array.size() == 1 &&
              query_result.futureOccurrences.array.size() == 1 && query_result.exceptions.array.size() == 1,
          "完整日程查询结果必须保留一次性、未来周期和例外条目");
    JsonValue query_root;
    Check(ParseJson(ReadFixture("schedule-query-result.json"), query_root).ok(),
          "查询结果 fixture 必须可作为拒绝测试基线");
    RequireScheduleQueryResultRejected(JsonValue::Array({}), "非对象查询结果必须被 C++ 拒绝");
    for (const auto& [name, invalid] : std::initializer_list<std::pair<const char*, JsonValue>>{
             {"schemaVersion", JsonValue::String("2")},
             {"businessEventId", JsonValue::String("")},
             {"correlationId", JsonValue::String("")},
             {"userId", JsonValue::Number(1)},
             {"deviceId", JsonValue::String("")},
             {"resultCount", JsonValue::String("2")},
             {"resultCount", JsonValue::Number(-1)},
             {"resultCount", JsonValue::Number(1001)},
             {"resultCount", JsonValue::Number(1.5)},
             {"schedules", JsonValue::String("invalid")},
             {"futureOccurrences", JsonValue::String("invalid")},
             {"exceptions", JsonValue::String("invalid")},
             {"queriedAt", JsonValue::String("invalid")},
         }) {
        JsonValue invalid_root = query_root;
        invalid_root.object[name] = invalid;
        RequireScheduleQueryResultRejected(invalid_root, "查询结果关键字段非法必须被 C++ 拒绝");
    }
    for (const auto& [field, value] : std::initializer_list<std::pair<const char*, JsonValue>>{
             {"status", JsonValue::String("unknown")},
             {"keyword", JsonValue::Number(1)},
             {"startDate", JsonValue::String("2026/08/03")},
             {"endDate", JsonValue::String("2026-08")},
         }) {
        JsonValue invalid_root = query_root;
        invalid_root.object["query"].object[field] = value;
        RequireScheduleQueryResultRejected(invalid_root, "查询范围字段非法必须被 C++ 拒绝");
    }
    JsonValue missing_query = query_root;
    missing_query.object.erase("query");
    RequireScheduleQueryResultRejected(missing_query, "缺少 query 对象必须被 C++ 拒绝");
    JsonValue mismatch_count = query_root;
    mismatch_count.object["resultCount"] = JsonValue::Number(0);
    RequireScheduleQueryResultRejected(mismatch_count, "条目数量不匹配必须被 C++ 拒绝");
    for (const char* field : {"schedules", "futureOccurrences", "exceptions"}) {
        JsonValue oversized = query_root;
        oversized.object[field] = JsonValue::Array(std::vector<JsonValue>(101, JsonValue::Object({})));
        if (std::string(field) == "schedules") {
            oversized.object["resultCount"] = JsonValue::Number(101);
        } else if (std::string(field) == "futureOccurrences") {
            oversized.object["resultCount"] = JsonValue::Number(102);
        }
        RequireScheduleQueryResultRejected(oversized, "超过 Gateway 上限的查询结果数组必须被 C++ 拒绝");
    }

    // 非法日程回执 fixture：与 TS 一致的拒绝语义
    RequireScheduleReceiptRejected("schedule-receipt-invalid-version.json", "非法版本日程回执必须被 C++ 拒绝");
    RequireScheduleReceiptRejected("schedule-receipt-invalid-enum.json", "非法枚举日程回执必须被 C++ 拒绝");
    RequireScheduleReceiptRejected("schedule-receipt-invalid-time.json", "非法时间日程回执必须被 C++ 拒绝");

    // 动作结果：字段与 TS parseReminderActionResult 一致
    ReminderActionResult action_result;
    Check(ParseActionResultFixture("reminder-action-result.json", action_result).ok(),
          "共享动作结果 fixture 必须被 C++ 解析");
    Check(action_result.schemaVersion == kDeviceContractVersion, "动作结果必须共享设备契约版本");
    Check(action_result.operationId == "operation-fixture", "动作结果 operationId 必须被保留");
    Check(action_result.reminderTriggerId == "trigger-fixture", "动作结果 reminderTriggerId 必须被保留");
    Check(action_result.status == "succeeded", "动作结果状态枚举必须与 TS 语义一致");
    Check(action_result.nextTriggerAt.has_value() && *action_result.nextTriggerAt == "2026-08-03T00:10:00.000Z",
          "可选 nextTriggerAt 必须被保留");

    // 非法动作结果 fixture：与 TS 一致的拒绝语义
    RequireActionResultRejected("reminder-action-result-invalid-status.json", "非法状态动作结果必须被 C++ 拒绝");
    RequireActionResultRejected("reminder-action-result-invalid-time.json", "非法时间动作结果必须被 C++ 拒绝");

    // 设备本地语音状态：字段、snooze 时间与来源必须与 TS 一致
    VoiceReminderActionStatus voice_status;
    Check(ParseVoiceStatusFixture("voice-reminder-action-status.json", voice_status).ok(),
          "共享语音状态 fixture 必须被 C++ 解析");
    Check(voice_status.schemaVersion == kDeviceContractVersion && voice_status.eventId == "voice-event-fixture" &&
              voice_status.deviceId == "device-fixture" && voice_status.reminderTriggerId == "trigger-fixture" &&
              voice_status.operationId == "voice-operation-fixture" && voice_status.action == "snooze" &&
              voice_status.status == "succeeded" && voice_status.source == "voice",
          "语音状态字段必须被完整保留");
    Check(voice_status.nextTriggerAt.has_value() && *voice_status.nextTriggerAt == "2026-08-03T00:11:00.000Z",
          "成功 snooze 的 nextTriggerAt 必须被保留");
    RequireVoiceStatusRejected("voice-reminder-action-status-invalid-source.json", "非法语音状态来源必须被 C++ 拒绝");

    // 动作命令：字段与 TS ReminderActionCommand 一致
    ReminderActionCommand command;
    Check(ParseCommandFixture("reminder-action-command.json", command).ok(), "共享动作命令 fixture 必须被 C++ 解析");
    Check(command.schemaVersion == kDeviceContractVersion, "动作命令必须共享设备契约版本");
    Check(command.commandId == "command-fixture" && command.operationId == "operation-fixture",
          "动作命令的命令/操作标识必须被保留");
    Check(command.correlationId == "correlation-fixture" && command.deviceId == "device-fixture",
          "动作命令的关联/设备标识必须被保留");
    Check(command.actorBindingId == "binding-fixture" && command.reminderTriggerId == "trigger-fixture",
          "动作命令的绑定/触发标识必须被保留");
    Check(command.action == "snooze" && command.minutes.has_value() && *command.minutes == 10,
          "动作类型与推迟分钟数必须与 TS 语义一致");
    Check(command.occurredAt == "2026-08-03T00:00:00.000Z" && command.expiresAt == "2026-08-03T00:10:00.000Z",
          "动作命令时间字段必须被保留");

    // 非法动作命令 fixture：与 TS 一致的拒绝语义
    RequireCommandRejected("reminder-action-command-invalid-action.json", "非法动作类型命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-invalid-time.json", "非法时间命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-missing-field.json", "缺字段命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-invalid-acknowledge-params.json",
                           "acknowledge 命令携带 params 必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-invalid-not-object.json", "非对象命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-invalid-version.json", "非法版本命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-missing-operation.json", "缺 operationId 命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-missing-correlation.json", "缺 correlationId 命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-missing-device.json", "缺 deviceId 命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-missing-binding.json", "缺 actorBindingId 命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-missing-trigger.json", "缺 reminderTriggerId 命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-invalid-params-type.json", "params 非对象命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-invalid-params-minutes.json",
                           "params 缺 minutes 命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-invalid-params-fraction.json",
                           "params 非整数分钟命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-invalid-params-overflow.json",
                           "params 超上限分钟命令必须被 C++ 拒绝");
    RequireCommandRejected("reminder-action-command-invalid-occurred-at.json", "非法发生时间命令必须被 C++ 拒绝");

    // 通知受理结果：强提醒携带 actionStream 窗口，弱提醒不携带
    NotificationSubmission strong_submission;
    Check(ParseSubmissionFixture("notification-submission.json", strong_submission).ok(),
          "共享强提醒受理结果 fixture 必须被 C++ 解析");
    Check(strong_submission.businessEventId == "event-fixture" && strong_submission.status == "accepted",
          "受理结果业务事件标识与状态必须被保留");
    Check(strong_submission.deliveries.size() == 1 &&
              strong_submission.deliveries[0].deliveryId == "delivery-fixture" &&
              strong_submission.deliveries[0].bindingId == "binding-fixture" &&
              strong_submission.deliveries[0].status == "pending",
          "受理结果交付行必须被保留");
    Check(strong_submission.actionStream.has_value() &&
              strong_submission.actionStream->reminderTriggerId == "trigger-fixture" &&
              strong_submission.actionStream->expiresAt == "2026-08-03T00:10:00.000Z",
          "强提醒受理结果必须携带 actionStream 窗口");

    NotificationSubmission weak_submission;
    Check(ParseSubmissionFixture("notification-submission-weak.json", weak_submission).ok(),
          "共享弱提醒受理结果 fixture 必须被 C++ 解析");
    Check(!weak_submission.actionStream.has_value() && weak_submission.deliveries.empty(),
          "弱提醒受理结果不得携带 actionStream");

    // 非法受理结果 fixture：与 TS 一致的拒绝语义
    RequireSubmissionRejected("notification-submission-invalid-status.json", "非法状态受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-invalid-time.json", "非法时间受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-missing-field.json", "缺字段受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-invalid-not-object.json", "非对象受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-invalid-deliveries.json",
                              "deliveries 非数组受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-invalid-delivery-shape.json",
                              "交付行非对象受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-invalid-delivery-id.json",
                              "交付行缺 deliveryId 受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-invalid-delivery-binding.json",
                              "交付行缺 bindingId 受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-invalid-delivery-status.json",
                              "交付行非法状态受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-invalid-actionstream-shape.json",
                              "actionStream 非对象受理结果必须被 C++ 拒绝");
    RequireSubmissionRejected("notification-submission-invalid-actionstream-field.json",
                              "actionStream 缺 reminderTriggerId 受理结果必须被 C++ 拒绝");
    return 0;
}
