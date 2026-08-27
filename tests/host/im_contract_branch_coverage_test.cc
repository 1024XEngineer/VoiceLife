#include <functional>
#include <string_view>
#include <vector>

#include "support/test_support.h"
#include "voicelife/contracts/im/notification_submission.h"
#include "voicelife/contracts/im/pairing_session.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/reminder_action_status_report.h"
#include "voicelife/contracts/im/schedule_query_result.h"
#include "voicelife/contracts/im/schedule_receipt.h"
#include "voicelife/contracts/json.h"

using voicelife::JsonValue;
using voicelife::Status;
using voicelife::contracts::im::NotificationSubmission;
using voicelife::contracts::im::PairingSessionStatus;
using voicelife::contracts::im::ReminderActionResult;
using voicelife::contracts::im::ReminderActionStatusReport;
using voicelife::contracts::im::ScheduleQueryResultIntent;
using voicelife::contracts::im::ScheduleReceiptIntent;
using voicelife::test::Check;

namespace {

JsonValue Document(std::string_view text) {
    JsonValue value;
    Check(voicelife::ParseJson(text, value).ok(), "分支覆盖测试 JSON 必须合法");
    return value;
}

template <typename Output>
void Reject(JsonValue value, const std::function<Status(const JsonValue&, Output&)>& parser) {
    Output output;
    const auto status = parser(value, output);
    Check(!status.ok(), "非法契约分支必须被拒绝");
}

const char kReceipt[] = R"json({
  "schemaVersion":"1","eventId":"event","correlationId":"correlation","userId":"user",
  "deviceId":"device","operationType":"created","scheduleId":"schedule","result":"succeeded",
  "summary":"summary","occurredAt":"2026-08-03T00:00:00Z"
})json";

const char kSubmission[] = R"json({
  "businessEventId":"event","status":"accepted",
  "deliveries":[{"deliveryId":"delivery","bindingId":"binding","status":"pending"}],
  "actionStream":{"reminderTriggerId":"trigger","expiresAt":"2026-08-03T00:10:00Z"}
})json";

const char kActionResult[] = R"json({
  "schemaVersion":"1","operationId":"operation","reminderTriggerId":"trigger","status":"succeeded",
  "nextTriggerAt":"2026-08-03T00:10:00Z","errorCode":"retry","details":{"attempt":1},
  "occurredAt":"2026-08-03T00:00:00Z"
})json";

const char kStatusReport[] = R"json({
  "schemaVersion":"1","eventId":"event","correlationId":"correlation","deviceId":"device",
  "reminderTriggerId":"trigger","operationId":"operation","action":"snooze","status":"succeeded",
  "occurredAt":"2026-08-03T00:00:00Z","nextTriggerAt":"2026-08-03T00:10:00Z",
  "errorCode":"retry","details":{"attempt":1},"source":"voice"
})json";

const char kQuery[] = R"json({
  "schemaVersion":"1","businessEventId":"event","correlationId":"correlation","userId":"user",
  "deviceId":"device","query":{"keyword":"keyword","status":"active","startDate":"2026-08-01",
  "endDate":"2026-08-31"},"resultCount":1,"schedules":[{}],"futureOccurrences":[],"exceptions":[],
  "queriedAt":"2026-08-03T00:00:00Z"
})json";

const char kPairingStatus[] = R"json({
  "id":"session","userId":"user","deviceId":"device","allowedPlatforms":["feishu"],
  "status":"confirmed","expiresAt":"2026-08-03T00:10:00Z","createdAt":"2026-08-03T00:00:00Z",
  "confirmedAt":"2026-08-03T00:05:00Z"
})json";

void CheckReceiptBranches() {
    for (const char* field : {"eventId", "correlationId", "deviceId", "scheduleId", "summary"}) {
        auto value = Document(kReceipt);
        value.object[field] = JsonValue::Number(1);
        Reject<ScheduleReceiptIntent>(std::move(value), [](const JsonValue& root, ScheduleReceiptIntent& out) {
            return ParseScheduleReceiptIntent(root, out);
        });
    }
    for (const char* operation : {"updated", "cancelled", "undone", "invalid"}) {
        auto value = Document(kReceipt);
        value.object["operationType"] = JsonValue::String(operation);
        if (std::string_view(operation) == "invalid") {
            Reject<ScheduleReceiptIntent>(std::move(value), [](const JsonValue& root, ScheduleReceiptIntent& out) {
                return ParseScheduleReceiptIntent(root, out);
            });
        } else {
            ScheduleReceiptIntent output;
            Check(ParseScheduleReceiptIntent(value, output).ok(), "所有合法操作类型都应被接受");
        }
    }
    auto invalid_user = Document(kReceipt);
    invalid_user.object["userId"] = JsonValue::Number(1);
    Reject<ScheduleReceiptIntent>(std::move(invalid_user), [](const JsonValue& root, ScheduleReceiptIntent& out) {
        return ParseScheduleReceiptIntent(root, out);
    });
    auto invalid_occurred = Document(kReceipt);
    invalid_occurred.object["occurredAt"] = JsonValue::String("bad");
    Reject<ScheduleReceiptIntent>(std::move(invalid_occurred), [](const JsonValue& root, ScheduleReceiptIntent& out) {
        return ParseScheduleReceiptIntent(root, out);
    });
}

void CheckSubmissionBranches() {
    for (const char* field : {"businessEventId", "status", "deliveries"}) {
        auto value = Document(kSubmission);
        value.object[field] = JsonValue::Number(1);
        Reject<NotificationSubmission>(std::move(value), [](const JsonValue& root, NotificationSubmission& out) {
            return ParseNotificationSubmission(root, out);
        });
    }
    for (const char* field : {"deliveryId", "bindingId", "status"}) {
        auto value = Document(kSubmission);
        value.object["deliveries"].array[0].object[field] = JsonValue::Number(1);
        Reject<NotificationSubmission>(std::move(value), [](const JsonValue& root, NotificationSubmission& out) {
            return ParseNotificationSubmission(root, out);
        });
    }
    for (const char* field : {"reminderTriggerId", "expiresAt"}) {
        auto value = Document(kSubmission);
        value.object["actionStream"].object[field] = JsonValue::Number(1);
        Reject<NotificationSubmission>(std::move(value), [](const JsonValue& root, NotificationSubmission& out) {
            return ParseNotificationSubmission(root, out);
        });
    }
    auto non_object_stream = Document(kSubmission);
    non_object_stream.object["actionStream"] = JsonValue::Array({});
    Reject<NotificationSubmission>(
        std::move(non_object_stream),
        [](const JsonValue& root, NotificationSubmission& out) { return ParseNotificationSubmission(root, out); });
    auto no_stream = Document(kSubmission);
    no_stream.object.erase("actionStream");
    NotificationSubmission output;
    Check(ParseNotificationSubmission(no_stream, output).ok() && !output.actionStream.has_value(),
          "缺失 actionStream 的受理结果应被接受");
}

void CheckActionResultBranches() {
    for (const char* field : {"operationId", "reminderTriggerId", "occurredAt"}) {
        auto value = Document(kActionResult);
        value.object[field] = JsonValue::Number(1);
        Reject<ReminderActionResult>(std::move(value), [](const JsonValue& root, ReminderActionResult& out) {
            return ParseReminderActionResult(root, out);
        });
    }
    for (const char* status : {"retryable_failed", "failed", "expired", "invalid"}) {
        auto value = Document(kActionResult);
        value.object["status"] = JsonValue::String(status);
        if (std::string_view(status) == "invalid") {
            Reject<ReminderActionResult>(std::move(value), [](const JsonValue& root, ReminderActionResult& out) {
                return ParseReminderActionResult(root, out);
            });
        } else {
            ReminderActionResult output;
            Check(ParseReminderActionResult(value, output).ok(), "所有合法动作结果状态都应被接受");
        }
    }
    for (const char* field : {"nextTriggerAt", "errorCode"}) {
        auto value = Document(kActionResult);
        value.object[field] = JsonValue::Bool(true);
        Reject<ReminderActionResult>(std::move(value), [](const JsonValue& root, ReminderActionResult& out) {
            return ParseReminderActionResult(root, out);
        });
    }
    auto oversized_details = Document(kActionResult);
    oversized_details.object["details"] = JsonValue::Array(std::vector<JsonValue>(33, JsonValue::Number(1)));
    Check(oversized_details.object["details"].array.size() > 16, "details 变体必须超出预算");
    Reject<ReminderActionResult>(std::move(oversized_details), [](const JsonValue& root, ReminderActionResult& out) {
        return ParseReminderActionResult(root, out);
    });
}

void CheckStatusReportBranches() {
    for (const char* field : {"eventId", "correlationId", "deviceId", "reminderTriggerId", "operationId", "source"}) {
        auto value = Document(kStatusReport);
        value.object[field] = JsonValue::Number(1);
        Reject<ReminderActionStatusReport>(std::move(value),
                                           [](const JsonValue& root, ReminderActionStatusReport& out) {
                                               return ParseReminderActionStatusReport(root, out);
                                           });
    }
    auto acknowledge = Document(kStatusReport);
    acknowledge.object["action"] = JsonValue::String("acknowledge");
    acknowledge.object.erase("nextTriggerAt");
    ReminderActionStatusReport acknowledge_output;
    Check(ParseReminderActionStatusReport(acknowledge, acknowledge_output).ok(),
          "acknowledge 成功报告可以省略 nextTriggerAt");
    auto invalid_action = Document(kStatusReport);
    invalid_action.object["action"] = JsonValue::String("invalid");
    Reject<ReminderActionStatusReport>(std::move(invalid_action),
                                       [](const JsonValue& root, ReminderActionStatusReport& out) {
                                           return ParseReminderActionStatusReport(root, out);
                                       });
    for (const char* field : {"nextTriggerAt", "errorCode"}) {
        auto value = Document(kStatusReport);
        value.object[field] = JsonValue::Bool(true);
        Reject<ReminderActionStatusReport>(std::move(value),
                                           [](const JsonValue& root, ReminderActionStatusReport& out) {
                                               return ParseReminderActionStatusReport(root, out);
                                           });
    }
    auto oversized_details = Document(kStatusReport);
    oversized_details.object["details"] = JsonValue::Array(std::vector<JsonValue>(33, JsonValue::Number(1)));
    Reject<ReminderActionStatusReport>(std::move(oversized_details),
                                       [](const JsonValue& root, ReminderActionStatusReport& out) {
                                           return ParseReminderActionStatusReport(root, out);
                                       });
    auto invalid_source = Document(kStatusReport);
    invalid_source.object["source"] = JsonValue::String("device");
    Reject<ReminderActionStatusReport>(std::move(invalid_source),
                                       [](const JsonValue& root, ReminderActionStatusReport& out) {
                                           return ParseReminderActionStatusReport(root, out);
                                       });
    auto inconsistent = Document(kStatusReport);
    inconsistent.object["action"] = JsonValue::String("acknowledge");
    Reject<ReminderActionStatusReport>(std::move(inconsistent),
                                       [](const JsonValue& root, ReminderActionStatusReport& out) {
                                           return ParseReminderActionStatusReport(root, out);
                                       });
}

void CheckQueryBranches() {
    for (const char* field : {"businessEventId", "correlationId", "deviceId", "resultCount", "schedules",
                              "futureOccurrences", "exceptions", "queriedAt"}) {
        auto value = Document(kQuery);
        value.object[field] = std::string_view(field) == "resultCount" ? JsonValue::Number(0) : JsonValue::Number(1);
        Reject<ScheduleQueryResultIntent>(std::move(value), [](const JsonValue& root, ScheduleQueryResultIntent& out) {
            return ParseScheduleQueryResultIntent(root, out);
        });
    }
    for (const char* field : {"keyword", "startDate", "endDate"}) {
        auto value = Document(kQuery);
        value.object["query"].object[field] = JsonValue::Number(1);
        Reject<ScheduleQueryResultIntent>(std::move(value), [](const JsonValue& root, ScheduleQueryResultIntent& out) {
            return ParseScheduleQueryResultIntent(root, out);
        });
    }
    for (const char* status : {"all", "cancelled", "completed", "invalid"}) {
        auto value = Document(kQuery);
        value.object["query"].object["status"] = JsonValue::String(status);
        if (std::string_view(status) == "invalid") {
            Reject<ScheduleQueryResultIntent>(std::move(value),
                                              [](const JsonValue& root, ScheduleQueryResultIntent& out) {
                                                  return ParseScheduleQueryResultIntent(root, out);
                                              });
        } else {
            ScheduleQueryResultIntent output;
            Check(ParseScheduleQueryResultIntent(value, output).ok(), "所有合法查询状态都应被接受");
        }
    }
    auto mismatch = Document(kQuery);
    mismatch.object["resultCount"] = JsonValue::Number(0);
    Reject<ScheduleQueryResultIntent>(std::move(mismatch), [](const JsonValue& root, ScheduleQueryResultIntent& out) {
        return ParseScheduleQueryResultIntent(root, out);
    });
}

void CheckPairingBranches() {
    for (const char* field : {"id", "deviceId", "status", "expiresAt", "createdAt"}) {
        auto value = Document(kPairingStatus);
        value.object[field] = JsonValue::Number(1);
        Reject<PairingSessionStatus>(std::move(value), [](const JsonValue& root, PairingSessionStatus& out) {
            return ParsePairingSessionStatus(root, out);
        });
    }
    for (const char* platform : {"wechat_official", "dingtalk", "invalid"}) {
        auto value = Document(kPairingStatus);
        value.object["allowedPlatforms"].array[0] = JsonValue::String(platform);
        if (std::string_view(platform) == "invalid") {
            Reject<PairingSessionStatus>(std::move(value), [](const JsonValue& root, PairingSessionStatus& out) {
                return ParsePairingSessionStatus(root, out);
            });
        } else {
            PairingSessionStatus output;
            Check(ParsePairingSessionStatus(value, output).ok(), "所有合法配对平台都应被接受");
        }
    }
    auto no_platforms = Document(kPairingStatus);
    no_platforms.object["allowedPlatforms"] = JsonValue::Array({});
    Reject<PairingSessionStatus>(std::move(no_platforms), [](const JsonValue& root, PairingSessionStatus& out) {
        return ParsePairingSessionStatus(root, out);
    });
    auto pending = Document(kPairingStatus);
    pending.object["status"] = JsonValue::String("pending");
    pending.object.erase("confirmedAt");
    PairingSessionStatus output;
    Check(ParsePairingSessionStatus(pending, output).ok(), "pending 状态缺少 confirmedAt 应被接受");
    auto invalid_window = Document(kPairingStatus);
    invalid_window.object["confirmedAt"] = JsonValue::String("2026-08-03T00:10:00Z");
    Reject<PairingSessionStatus>(std::move(invalid_window), [](const JsonValue& root, PairingSessionStatus& out) {
        return ParsePairingSessionStatus(root, out);
    });
}

}  // namespace

int main() {
    CheckReceiptBranches();
    CheckSubmissionBranches();
    CheckActionResultBranches();
    CheckStatusReportBranches();
    CheckQueryBranches();
    CheckPairingBranches();
    return 0;
}
