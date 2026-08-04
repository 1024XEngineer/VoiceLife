#include <string>
#include <string_view>

#include "support/test_support.h"
#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/schedule_receipt.h"
#include "voicelife/contracts/json.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::Status;
using voicelife::contracts::im::NotificationIntent;
using voicelife::contracts::im::ParseNotificationIntent;
using voicelife::contracts::im::ParseReminderActionResult;
using voicelife::contracts::im::ParseScheduleReceiptIntent;
using voicelife::contracts::im::ReminderActionResult;
using voicelife::contracts::im::ScheduleReceiptIntent;
using voicelife::test::Check;

namespace {

JsonValue ParseDocument(std::string_view input) {
    JsonValue root;
    Check(voicelife::ParseJson(input, root).ok(), "测试 JSON 应解析成功");
    return root;
}

void RequireNotificationRejected(std::string_view json) {
    NotificationIntent out;
    const Status status = ParseNotificationIntent(ParseDocument(json), out);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, "非法通知契约应被拒绝");
}

void RequireScheduleRejected(std::string_view json) {
    ScheduleReceiptIntent out;
    const Status status = ParseScheduleReceiptIntent(ParseDocument(json), out);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, "非法日程回执应被拒绝");
}

void RequireResultRejected(std::string_view json) {
    ReminderActionResult out;
    const Status status = ParseReminderActionResult(ParseDocument(json), out);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, "非法动作结果应被拒绝");
}

}  // namespace

int main() {
    // ===== NotificationIntent 校验分支 =====
    RequireNotificationRejected("[]");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"2\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"urgent\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":{},\"occurredAt\":"
        "\"2026-01-01T00:00:00Z\"}");
    // 弱提醒带动作、强提醒空动作
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
        "\"actions\":[{\"kind\":\"command\",\"type\":\"acknowledge\",\"label\":\"知道了\"}],\"occurredAt\":"
        "\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],\"occurredAt\":"
        "\"2026-01-01T00:00:00Z\"}");
    // 动作字段非法
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[42],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
        "\"actions\":[{\"kind\":\"script\",\"type\":\"acknowledge\",\"label\":\"x\"}],\"occurredAt\":"
        "\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
        "\"actions\":[{\"kind\":\"command\",\"type\":\"dismiss\",\"label\":\"x\"}],\"occurredAt\":"
        "\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
        "\"actions\":[{\"kind\":\"command\",\"type\":\"snooze\"}],\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
        "\"actions\":[{\"kind\":\"command\",\"type\":\"snooze\",\"label\":\"x\",\"params\":{\"minutes\":0}}],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    // 必填字段缺失 / 类型错误
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"correlationId\":\"c\",\"kind\":\"reminder_due\",\"recipient\":{\"userId\":\"u\","
        "\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\",\"instanceId\":\"i\",\"reminderTriggerId\":\"r\","
        "\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},\"plannedAt\":\"2026-01-01T00:00:00Z\","
        "\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"nope\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\"},\"scheduleId\":\"s\",\"taskId\":\"t\",\"instanceId\":\"i\","
        "\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{},\"plannedAt\":"
        "\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],\"occurredAt\":"
        "\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"not-a-time\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],\"occurredAt\":"
        "\"2026-01-01T00:00:00Z\"}");
    // 其余必填字段缺失 / 类型错误分支
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"kind\":\"reminder_due\",\"recipient\":{\"userId\":\"u\","
        "\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\",\"instanceId\":\"i\",\"reminderTriggerId\":\"r\","
        "\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},\"plannedAt\":\"2026-01-01T00:00:00Z\","
        "\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"taskId\":\"t\",\"instanceId\":\"i\","
        "\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"instanceId\":\"i\","
        "\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":42,\"scheduleId\":\"s\",\"taskId\":\"t\",\"instanceId\":\"i\",\"reminderTriggerId\":\"r\","
        "\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},\"plannedAt\":\"2026-01-01T00:00:00Z\","
        "\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":42,"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
        "\"actions\":[{\"kind\":\"command\",\"type\":\"snooze\",\"label\":\"x\",\"params\":{\"minutes\":1.5}}],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"bad\",\"actions\":[],\"occurredAt\":"
        "\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
        "\"occurredAt\":\"bad\"}");

    // ===== ScheduleReceiptIntent 校验分支 =====
    RequireScheduleRejected("42");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"2\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"operationType\":\"rescheduled\",\"scheduleId\":\"s\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"pending\",\"summary\":\"x\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"2023-02-29T00:00:00Z\"}");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"correlationId\":\"c\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"u\","
        "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"operationType\":\"created\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"succeeded\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    // 可选 userId 缺失仍应合法
    ScheduleReceiptIntent without_user;
    Check(
        ParseScheduleReceiptIntent(
            ParseDocument("{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"deviceId\":\"d\","
                          "\"operationType\":\"updated\",\"scheduleId\":\"s\",\"result\":\"failed\",\"summary\":\"x\","
                          "\"occurredAt\":\"2024-02-29T00:00:00Z\"}"),
            without_user)
                .ok() &&
            !without_user.userId.has_value(),
        "可选 userId 缺失与闰日应被接受");

    // ===== ReminderActionResult 校验分支 =====
    RequireResultRejected("null");
    RequireResultRejected(
        "{\"schemaVersion\":\"2\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\",\"status\":\"succeeded\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"reminderTriggerId\":\"r\",\"status\":\"succeeded\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"status\":\"succeeded\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\",\"status\":\"suspended\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\",\"status\":\"succeeded\","
        "\"nextTriggerAt\":\"bad\",\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\",\"status\":\"expired\","
        "\"occurredAt\":\"2026-01-01T24:00:00Z\"}");
    // 可选字段齐全应合法
    ReminderActionResult full;
    Check(ParseReminderActionResult(
              ParseDocument("{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\","
                            "\"status\":\"retryable_failed\",\"errorCode\":\"e1\",\"details\":{\"attempt\":2},"
                            "\"occurredAt\":\"2026-01-01T00:00:00Z\"}"),
              full)
                  .ok() &&
              full.errorCode.has_value() && full.details.has_value(),
          "可选 errorCode 与 details 应被接受");
    return 0;
}
