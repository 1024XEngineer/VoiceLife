#include <string>
#include <string_view>

#include "support/test_support.h"
#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/reminder_action_status_report.h"
#include "voicelife/contracts/im/schedule_receipt.h"
#include "voicelife/contracts/json.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::Status;
using voicelife::contracts::im::NotificationIntent;
using voicelife::contracts::im::ParseNotificationIntent;
using voicelife::contracts::im::ParseReminderActionResult;
using voicelife::contracts::im::ParseReminderActionStatusReport;
using voicelife::contracts::im::ParseScheduleReceiptIntent;
using voicelife::contracts::im::ReminderActionResult;
using voicelife::contracts::im::ReminderActionStatusReport;
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

void RequireScheduleTimeRejected(const std::string& time) {
    const std::string json =
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"" +
        time + "\"}";
    RequireScheduleRejected(json);
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
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
        "\"actions\":[{\"type\":\"acknowledge\",\"label\":\"x\"}],\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
        "\"actions\":[{\"kind\":\"command\",\"label\":\"x\"}],\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
        "\"actions\":[{\"kind\":\"command\",\"type\":\"snooze\",\"label\":\"x\",\"params\":42}],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"weak\",\"content\":{\"title\":\"x\"},"
        "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[],"
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
    // 带可选 body 的合法通知
    NotificationIntent with_body;
    Check(ParseNotificationIntent(
              ParseDocument("{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\","
                            "\"kind\":\"reminder_due\",\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},"
                            "\"scheduleId\":\"s\",\"taskId\":\"t\",\"instanceId\":\"i\",\"reminderTriggerId\":\"r\","
                            "\"reminderType\":\"weak\",\"content\":{\"title\":\"x\",\"body\":\"y\"},"
                            "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
                            "\"actions\":[],\"occurredAt\":\"2026-01-01T00:00:00Z\"}"),
              with_body)
                  .ok() &&
              with_body.content.body.has_value(),
          "可选 content.body 应被接受");
    // 有效动作参数边界：acknowledge 允许保留参数，snooze 接受最大延迟值。
    NotificationIntent boundary_actions;
    Check(ParseNotificationIntent(ParseDocument(R"json({
                "schemaVersion":"1","businessEventId":"e","correlationId":"c",
                "kind":"reminder_due","recipient":{"userId":"u","deviceId":"d"},
                "scheduleId":"s","taskId":"t","instanceId":"i","reminderTriggerId":"r",
                "reminderType":"strong","content":{"title":"x"},
                "plannedAt":"2026-01-01T00:00:00Z","triggerAt":"2026-01-01T00:00:00Z",
                "actions":[
                  {"kind":"command","type":"acknowledge","label":"知道了","params":{"minutes":1}},
                  {"kind":"command","type":"snooze","label":"推迟","params":{"minutes":1440}}
                ],"occurredAt":"2026-01-01T00:00:00Z"
              })json"),
                                  boundary_actions)
                  .ok() &&
              boundary_actions.actions.size() == 2 && boundary_actions.actions[0].minutes == 1 &&
              boundary_actions.actions[1].minutes == 1440,
          "有效动作参数边界应被接受");
    // snooze minutes 与动作数量必须受设备侧预算约束。
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\","
        "\"content\":{\"title\":\"x\"},\"plannedAt\":\"2026-01-01T00:00:00Z\","
        "\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[{\"kind\":\"command\","
        "\"type\":\"snooze\",\"label\":\"x\",\"params\":{\"minutes\":2147483648}}],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireNotificationRejected(
        "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\",\"kind\":\"reminder_due\","
        "\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},\"scheduleId\":\"s\",\"taskId\":\"t\","
        "\"instanceId\":\"i\",\"reminderTriggerId\":\"r\",\"reminderType\":\"strong\","
        "\"content\":{\"title\":\"x\"},\"plannedAt\":\"2026-01-01T00:00:00Z\","
        "\"triggerAt\":\"2026-01-01T00:00:00Z\",\"actions\":[{\"kind\":\"command\","
        "\"type\":\"snooze\",\"label\":\"x\",\"params\":{\"minutes\":1441}}],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    {
        std::string actions;
        for (int i = 0; i < 17; ++i) {
            if (i != 0) actions += ',';
            actions += "{\"kind\":\"command\",\"type\":\"acknowledge\",\"label\":\"x\"}";
        }
        RequireNotificationRejected(
            "{\"schemaVersion\":\"1\",\"businessEventId\":\"e\",\"correlationId\":\"c\","
            "\"kind\":\"reminder_due\",\"recipient\":{\"userId\":\"u\",\"deviceId\":\"d\"},"
            "\"scheduleId\":\"s\",\"taskId\":\"t\",\"instanceId\":\"i\",\"reminderTriggerId\":\"r\","
            "\"reminderType\":\"strong\",\"content\":{\"title\":\"x\"},"
            "\"plannedAt\":\"2026-01-01T00:00:00Z\",\"triggerAt\":\"2026-01-01T00:00:00Z\","
            "\"actions\":[" +
            actions + "],\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    }
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
    // 带时区偏移的合法时间
    ScheduleReceiptIntent offset;
    Check(ParseScheduleReceiptIntent(
              ParseDocument("{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"deviceId\":\"d\","
                            "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"succeeded\","
                            "\"summary\":\"x\",\"occurredAt\":\"2026-01-01T00:00:00+08:00\"}"),
              offset)
              .ok(),
          "带时区偏移的时间应被接受");
    // ISO 8601 各非法变体分支
    RequireScheduleTimeRejected("2026-01-01T00:00");
    RequireScheduleTimeRejected("2026-01-01T00:00:00");
    RequireScheduleTimeRejected("2026-01-01T00:00:00+0100");
    RequireScheduleTimeRejected("2026-01-01T00:00:00+24:00");
    RequireScheduleTimeRejected("2026-01-01T00:00:00+00:60");
    RequireScheduleTimeRejected("2026-01-01T00:00:00.1234567890Z");
    RequireScheduleTimeRejected("2026-13-01T00:00:00Z");
    RequireScheduleTimeRejected("2026-01-00T00:00:00Z");
    RequireScheduleTimeRejected("2026-01-32T00:00:00Z");
    RequireScheduleTimeRejected("2026-01-01T00:60:00Z");
    RequireScheduleTimeRejected("2026-01-01T00:00:60Z");
    RequireScheduleTimeRejected("2026-1-01T00:00:00Z");
    RequireScheduleTimeRejected("2026-01-01 00:00:00Z");
    RequireScheduleTimeRejected("2026-01-01T00:00:00X");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"u\",\"deviceId\":\"d\","
        "\"scheduleId\":\"s\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireScheduleRejected(
        "{\"schemaVersion\":\"1\",\"eventId\":\"e\",\"correlationId\":\"c\",\"userId\":\"\",\"deviceId\":\"d\","
        "\"operationType\":\"created\",\"scheduleId\":\"s\",\"result\":\"succeeded\",\"summary\":\"x\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
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
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\",\"status\":\"succeeded\","
        "\"errorCode\":\"\",\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\",\"status\":\"suspended\","
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\",\"status\":\"succeeded\","
        "\"nextTriggerAt\":\"bad\",\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\",\"status\":\"expired\","
        "\"occurredAt\":\"2026-01-01T24:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\","
        "\"status\":\"failed\",\"details\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\","
        "\"status\":\"failed\",\"details\":\"" +
        std::string(1025, 'x') + "\",\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\","
        "\"status\":\"failed\",\"details\":[[[[[0]]]]],"
        "\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
    RequireResultRejected(
        "{\"schemaVersion\":\"1\",\"operationId\":\"o\",\"reminderTriggerId\":\"r\","
        "\"status\":\"failed\",\"details\":{\"" +
        std::string(1025, 'k') + "\":0},\"occurredAt\":\"2026-01-01T00:00:00Z\"}");
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
    // 输出对象可安全复用：成功时整体替换，失败时保持原值。
    NotificationIntent reused_notification;
    reused_notification.content.body = "旧正文";
    reused_notification.actions.push_back({"command", "acknowledge", "旧动作", std::nullopt});
    Check(ParseNotificationIntent(ParseDocument(R"json({
                "schemaVersion":"1","businessEventId":"new-event","correlationId":"new-correlation",
                "kind":"reminder_due","recipient":{"userId":"new-user","deviceId":"new-device"},
                "scheduleId":"new-schedule","taskId":"new-task","instanceId":"new-instance",
                "reminderTriggerId":"new-trigger","reminderType":"weak","content":{"title":"新标题"},
                "plannedAt":"2026-01-01T00:00:00Z","triggerAt":"2026-01-01T00:00:00Z","actions":[],
                "occurredAt":"2026-01-01T00:00:00Z"
              })json"),
                                  reused_notification)
                  .ok() &&
              reused_notification.actions.empty() && !reused_notification.content.body.has_value(),
          "复用通知输出时应清除旧 actions 与可选 body");
    Check(!ParseNotificationIntent(ParseDocument(R"json({
                 "schemaVersion":"1","businessEventId":"invalid-event","correlationId":"invalid-correlation",
                 "kind":"reminder_due","recipient":{"userId":"u","deviceId":"d"},"scheduleId":"s",
                 "taskId":"t","instanceId":"i","reminderTriggerId":"r","reminderType":"weak",
                 "content":{"title":"非法通知"},"plannedAt":"2026-01-01T00:00:00Z",
                 "triggerAt":"2026-01-01T00:00:00Z","actions":[],"occurredAt":"bad"
               })json"),
                                   reused_notification)
                  .ok() &&
              reused_notification.businessEventId == "new-event" && reused_notification.content.title == "新标题",
          "通知解析失败时不应改写已有输出");
    ScheduleReceiptIntent reused_receipt;
    Check(ParseScheduleReceiptIntent(ParseDocument(R"json({
                "schemaVersion":"1","eventId":"with-user","correlationId":"c","userId":"old-user",
                "deviceId":"d","operationType":"created","scheduleId":"s","result":"succeeded",
                "summary":"有用户","occurredAt":"2026-01-01T00:00:00Z"
              })json"),
                                     reused_receipt)
              .ok(),
          "带 userId 的日程回执应解析成功");
    Check(ParseScheduleReceiptIntent(ParseDocument(R"json({
                "schemaVersion":"1","eventId":"without-user","correlationId":"c2","deviceId":"d2",
                "operationType":"updated","scheduleId":"s2","result":"failed","summary":"无用户",
                "occurredAt":"2026-01-02T00:00:00Z"
              })json"),
                                     reused_receipt)
                  .ok() &&
              !reused_receipt.userId.has_value() && reused_receipt.eventId == "without-user",
          "复用日程回执输出时应清除旧 userId");
    Check(!ParseScheduleReceiptIntent(ParseDocument(R"json({
                 "schemaVersion":"1","eventId":"invalid-event","correlationId":"invalid-correlation",
                 "userId":"invalid-user","deviceId":"invalid-device","operationType":"created",
                 "scheduleId":"invalid-schedule","result":"succeeded","summary":"非法回执","occurredAt":"bad"
               })json"),
                                      reused_receipt)
                  .ok() &&
              reused_receipt.eventId == "without-user" && !reused_receipt.userId.has_value() &&
              reused_receipt.summary == "无用户",
          "日程回执解析失败时不应改写已有输出");
    ReminderActionResult reused_result;
    Check(ParseReminderActionResult(ParseDocument(R"json({
                "schemaVersion":"1","operationId":"with-options","reminderTriggerId":"r",
                "status":"retryable_failed","nextTriggerAt":"2026-01-02T00:00:00Z","errorCode":"retry",
                "details":{"attempt":1},"occurredAt":"2026-01-01T00:00:00Z"
              })json"),
                                    reused_result)
              .ok(),
          "带可选字段的动作结果应解析成功");
    Check(ParseReminderActionResult(ParseDocument(R"json({
                "schemaVersion":"1","operationId":"without-options","reminderTriggerId":"r2",
                "status":"succeeded","occurredAt":"2026-01-02T00:00:00Z"
              })json"),
                                    reused_result)
                  .ok() &&
              !reused_result.nextTriggerAt.has_value() && !reused_result.errorCode.has_value() &&
              !reused_result.details.has_value(),
          "复用动作结果输出时应清除所有旧可选字段");
    Check(!ParseReminderActionResult(ParseDocument(R"json({
                 "schemaVersion":"1","operationId":"invalid-operation","reminderTriggerId":"invalid-trigger",
                 "status":"failed","errorCode":"invalid-error","details":{"changed":true},
                 "occurredAt":"bad"
               })json"),
                                     reused_result)
                  .ok() &&
              reused_result.operationId == "without-options" && !reused_result.errorCode.has_value() &&
              !reused_result.details.has_value(),
          "动作结果解析失败时不应改写已有输出");
    // 独立语音动作事实的非成功状态不受 nextTriggerAt 约束，但可选字段仍需严格校验。
    for (const char* status : {"retryable_failed", "failed", "expired"}) {
        const std::string json =
            std::string(R"json({"schemaVersion":"1","eventId":"e","correlationId":"c","deviceId":"d",
                "reminderTriggerId":"r","operationId":"o","action":"acknowledge","status":")json") +
            status + R"json(","occurredAt":"2026-01-01T00:00:00Z","source":"voice",
                "errorCode":"transport","details":{"attempt":1}})json";
        ReminderActionStatusReport report;
        Check(ParseReminderActionStatusReport(ParseDocument(json), report).ok() && report.status == status &&
                  report.errorCode.has_value() && report.details.has_value(),
              "非成功动作事实状态及可选字段应被接受");
    }
    const std::string valid_report = R"json({"schemaVersion":"1","eventId":"e","correlationId":"c","deviceId":"d",
        "reminderTriggerId":"r","operationId":"o","action":"snooze","status":"succeeded",
        "occurredAt":"2026-01-01T00:00:00Z","nextTriggerAt":"2026-01-01T00:10:00Z","source":"voice"})json";
    for (const char* key : {"nextTriggerAt", "errorCode"}) {
        JsonValue invalid = ParseDocument(valid_report);
        invalid.object[key] = JsonValue::Number(1);
        ReminderActionStatusReport report;
        Check(!ParseReminderActionStatusReport(invalid, report).ok(), "动作事实可选字段类型错误必须拒绝");
    }
    ReminderActionStatusReport scalar_details;
    JsonValue scalar_details_root = ParseDocument(valid_report);
    scalar_details_root.object["details"] = JsonValue::Number(1);
    Check(ParseReminderActionStatusReport(scalar_details_root, scalar_details).ok() &&
              scalar_details.details.has_value() && scalar_details.details->number == 1,
          "动作事实 details 应接受受限 JSON 标量");
    return 0;
}
