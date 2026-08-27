#include <functional>
#include <limits>
#include <string_view>
#include <vector>

#include "support/test_support.h"
#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/im/notification_submission.h"
#include "voicelife/contracts/im/pairing_session.h"
#include "voicelife/contracts/im/reminder_action_command.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/reminder_action_status_report.h"
#include "voicelife/contracts/im/schedule_query_result.h"
#include "voicelife/contracts/im/schedule_receipt.h"
#include "voicelife/contracts/json.h"

using voicelife::JsonValue;
using voicelife::Status;
using voicelife::contracts::im::NotificationIntent;
using voicelife::contracts::im::NotificationSubmission;
using voicelife::contracts::im::PairingSessionStatus;
using voicelife::contracts::im::ParseNotificationIntent;
using voicelife::contracts::im::ParseReminderActionCommand;
using voicelife::contracts::im::ReminderActionCommand;
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

const char kNotificationWeak[] = R"json({
  "schemaVersion":"1","businessEventId":"event","correlationId":"correlation","kind":"reminder_due",
  "recipient":{"userId":"user","deviceId":"device"},"scheduleId":"schedule","taskId":"task",
  "instanceId":"instance","reminderTriggerId":"trigger","reminderType":"weak","content":{"title":"title"},
  "plannedAt":"2026-08-03T00:00:00Z","triggerAt":"2026-08-03T00:00:00Z","actions":[],
  "occurredAt":"2026-08-03T00:00:00Z"
})json";

const char kNotificationStrong[] = R"json({
  "schemaVersion":"1","businessEventId":"event","correlationId":"correlation","kind":"reminder_due",
  "recipient":{"userId":"user","deviceId":"device"},"scheduleId":"schedule","taskId":"task",
  "instanceId":"instance","reminderTriggerId":"trigger","reminderType":"strong","content":{"title":"title"},
  "plannedAt":"2026-08-03T00:00:00Z","triggerAt":"2026-08-03T00:00:00Z",
  "actions":[{"kind":"command","type":"acknowledge","label":"ack"}],
  "occurredAt":"2026-08-03T00:00:00Z"
})json";

const char kActionCommand[] = R"json({
  "schemaVersion":"1","commandId":"command","operationId":"operation","correlationId":"correlation",
  "deviceId":"device","actorBindingId":"binding","reminderTriggerId":"trigger","action":"acknowledge",
  "occurredAt":"2026-08-03T00:00:00Z","expiresAt":"2026-08-03T00:10:00Z"
})json";

void CheckNotificationBranches() {
    auto weak = Document(kNotificationWeak);
    NotificationIntent weak_output;
    Check(ParseNotificationIntent(weak, weak_output).ok() && weak_output.actions.empty(), "弱提醒无动作时应被接受");

    auto strong = Document(kNotificationStrong);
    strong.object["actions"].array.push_back(
        Document(R"json({"kind":"command","type":"snooze","label":"snooze","params":{"minutes":1440}})json"));
    NotificationIntent strong_output;
    Check(ParseNotificationIntent(strong, strong_output).ok() && strong_output.actions.size() == 2,
          "强提醒应解析 acknowledge 与最大 snooze 动作");

    auto bad_recipient_user = Document(kNotificationWeak);
    bad_recipient_user.object["recipient"].object["userId"] = JsonValue::Number(1);
    Reject<NotificationIntent>(std::move(bad_recipient_user), [](const JsonValue& root, NotificationIntent& out) {
        return ParseNotificationIntent(root, out);
    });
    auto bad_kind = Document(kNotificationWeak);
    bad_kind.object["kind"] = JsonValue::Number(1);
    Reject<NotificationIntent>(std::move(bad_kind), [](const JsonValue& root, NotificationIntent& out) {
        return ParseNotificationIntent(root, out);
    });
    auto bad_body = Document(kNotificationWeak);
    bad_body.object["content"].object["body"] = JsonValue::String("");
    Reject<NotificationIntent>(std::move(bad_body), [](const JsonValue& root, NotificationIntent& out) {
        return ParseNotificationIntent(root, out);
    });
    auto bad_minutes = Document(kNotificationStrong);
    bad_minutes.object["actions"].array[0].object["type"] = JsonValue::String("snooze");
    bad_minutes.object["actions"].array[0].object["params"] =
        JsonValue::Object({{"minutes", JsonValue::Number(std::numeric_limits<double>::quiet_NaN())}});
    Reject<NotificationIntent>(std::move(bad_minutes), [](const JsonValue& root, NotificationIntent& out) {
        return ParseNotificationIntent(root, out);
    });
    auto too_many_actions = Document(kNotificationStrong);
    too_many_actions.object["actions"].array.resize(17, too_many_actions.object["actions"].array[0]);
    Reject<NotificationIntent>(std::move(too_many_actions), [](const JsonValue& root, NotificationIntent& out) {
        return ParseNotificationIntent(root, out);
    });
}

void CheckActionCommandBranches() {
    auto acknowledge = Document(kActionCommand);
    ReminderActionCommand acknowledge_output;
    Check(ParseReminderActionCommand(acknowledge, acknowledge_output).ok() &&
              acknowledge_output.action == "acknowledge" && !acknowledge_output.minutes.has_value(),
          "acknowledge 命令无参数时应被接受");
    auto snooze = Document(kActionCommand);
    snooze.object["action"] = JsonValue::String("snooze");
    snooze.object["params"] = JsonValue::Object({{"minutes", JsonValue::Number(1)}});
    ReminderActionCommand snooze_output;
    Check(ParseReminderActionCommand(snooze, snooze_output).ok() && snooze_output.minutes == 1,
          "snooze 命令最小分钟数应被接受");

    for (const char* field : {"commandId", "operationId", "correlationId", "deviceId", "actorBindingId",
                              "reminderTriggerId", "occurredAt", "expiresAt"}) {
        auto value = Document(kActionCommand);
        value.object[field] = JsonValue::Number(1);
        Reject<ReminderActionCommand>(std::move(value), [](const JsonValue& root, ReminderActionCommand& out) {
            return ParseReminderActionCommand(root, out);
        });
    }
    auto bad_params_type = Document(kActionCommand);
    bad_params_type.object["params"] = JsonValue::Array({});
    Reject<ReminderActionCommand>(std::move(bad_params_type), [](const JsonValue& root, ReminderActionCommand& out) {
        return ParseReminderActionCommand(root, out);
    });
    auto bad_minutes = Document(kActionCommand);
    bad_minutes.object["action"] = JsonValue::String("snooze");
    bad_minutes.object["params"] =
        JsonValue::Object({{"minutes", JsonValue::Number(std::numeric_limits<double>::quiet_NaN())}});
    Reject<ReminderActionCommand>(std::move(bad_minutes), [](const JsonValue& root, ReminderActionCommand& out) {
        return ParseReminderActionCommand(root, out);
    });
}

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
    for (const char* timestamp : {"2026-01-01T0x:00:00Z", "2026-01-01T00:00:x0Z", "2026-01-01T00:00:00+01:"}) {
        auto value = Document(kReceipt);
        value.object["occurredAt"] = JsonValue::String(timestamp);
        Reject<ScheduleReceiptIntent>(std::move(value), [](const JsonValue& root, ScheduleReceiptIntent& out) {
            return ParseScheduleReceiptIntent(root, out);
        });
    }
    auto fractional_offset = Document(kReceipt);
    fractional_offset.object["occurredAt"] = JsonValue::String("2026-01-01T00:00:00.1+08:30");
    ScheduleReceiptIntent fractional_output;
    Check(ParseScheduleReceiptIntent(fractional_offset, fractional_output).ok(), "带小数秒和时区偏移的时间应被接受");
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
    auto oversized_object = Document(kActionResult);
    oversized_object.object["details"] = JsonValue::Object({});
    for (int index = 0; index < 17; ++index) {
        oversized_object.object["details"].object["key" + std::to_string(index)] = JsonValue::Number(index);
    }
    Reject<ReminderActionResult>(std::move(oversized_object), [](const JsonValue& root, ReminderActionResult& out) {
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
    auto non_digit_date = Document(kQuery);
    non_digit_date.object["query"].object["startDate"] = JsonValue::String("2026-0x-03");
    Reject<ScheduleQueryResultIntent>(std::move(non_digit_date),
                                      [](const JsonValue& root, ScheduleQueryResultIntent& out) {
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
    auto fractional_offset = Document(kPairingStatus);
    fractional_offset.object["createdAt"] = JsonValue::String("2026-08-03T00:00:00.1+08:00");
    fractional_offset.object["confirmedAt"] = JsonValue::String("2026-08-03T00:05:00.2+08:00");
    fractional_offset.object["expiresAt"] = JsonValue::String("2026-08-03T00:10:00.000+08:00");
    PairingSessionStatus fractional_output;
    Check(ParsePairingSessionStatus(fractional_offset, fractional_output).ok(), "配对时间的短小数秒与时区偏移应被接受");
}

}  // namespace

int main() {
    CheckNotificationBranches();
    CheckActionCommandBranches();
    CheckReceiptBranches();
    CheckSubmissionBranches();
    CheckActionResultBranches();
    CheckStatusReportBranches();
    CheckQueryBranches();
    CheckPairingBranches();
    return 0;
}
