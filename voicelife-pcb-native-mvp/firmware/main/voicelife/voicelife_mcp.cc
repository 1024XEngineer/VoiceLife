#include "voicelife_mcp.h"

#include "voicelife_service.h"
#include "mcp_server.h"

namespace voicelife {
namespace {

constexpr char kCalendarCreateSchema[] = R"json({
  "type":"object",
  "properties":{
    "title":{"type":"string"},
    "startsAt":{"type":"string","description":"ISO 8601 timestamp with timezone"},
    "delayMinutes":{"type":"integer","minimum":1,"maximum":1440},
    "endsAt":{"type":"string"},
    "durationMinutes":{"type":"integer","minimum":1},
    "kind":{"type":"string","enum":["point","time_block"]},
    "remindAt":{"type":"string"},
    "weakReminder":{"type":"boolean"},
    "timeZone":{"type":"string"},
    "location":{"type":"string"},
    "notes":{"type":"string"},
    "recurrence":{"type":"object","properties":{"frequency":{"type":"string","enum":["daily","weekly","monthly"]},"weekday":{"type":"integer","minimum":1,"maximum":7},"monthDay":{"type":"integer","minimum":1,"maximum":31}},"required":["frequency"]},
    "conflictConfirmationToken":{"type":"string"}
  },
  "required":["title"]
})json";

constexpr char kCalendarQuerySchema[] = R"json({
  "type":"object",
  "properties":{"rangeStart":{"type":"string"},"rangeEnd":{"type":"string"}},
  "required":["rangeStart","rangeEnd"]
})json";

constexpr char kCalendarFindSchema[] = R"json({
  "type":"object",
  "properties":{"query":{"type":"string"},"rangeStart":{"type":"string"},"rangeEnd":{"type":"string"}},
  "required":["query"]
})json";

constexpr char kCalendarModifySchema[] = R"json({
  "type":"object",
  "properties":{
    "eventId":{"type":"string"},"scope":{"type":"string","enum":["this_occurrence","this_and_future","entire_series"]},
    "originalStartAt":{"type":"string"},"newStartAt":{"type":"string"},"startsAt":{"type":"string"},"endsAt":{"type":"string"},
    "title":{"type":"string"},"location":{"type":"string"},"notes":{"type":"string"},"weakReminder":{"type":"boolean"},"conflictConfirmationToken":{"type":"string"}
  },
  "required":["eventId"]
})json";

constexpr char kEventIdSchema[] = R"json({"type":"object","properties":{"eventId":{"type":"string"},"originalStartAt":{"type":"string"}},"required":["eventId"]})json";
constexpr char kConfirmEventSchema[] = R"json({"type":"object","properties":{"eventId":{"type":"string"},"confirmationToken":{"type":"string"}},"required":["eventId"]})json";
constexpr char kUndoSchema[] = R"json({"type":"object","properties":{"undoOperationId":{"type":"string"}},"required":["undoOperationId"]})json";
constexpr char kReminderIdSchema[] = R"json({"type":"object","properties":{"reminderId":{"type":"string"}},"required":["reminderId"]})json";
constexpr char kSnoozeSchema[] = R"json({"type":"object","properties":{"reminderId":{"type":"string"},"minutes":{"type":"integer","minimum":1,"maximum":1440}},"required":["reminderId","minutes"]})json";
constexpr char kNoteRecordSchema[] = R"json({"type":"object","properties":{"content":{"type":"string"},"category":{"type":"string"}},"required":["content"]})json";
constexpr char kNoteQuerySchema[] = R"json({"type":"object","properties":{"query":{"type":"string"}}})json";

}  // namespace

void VoiceLifeMcpAdapter::Register(VoiceLifeService& service) {
    auto& server = McpServer::GetInstance();
    server.AddJsonTool("calendar_create",
        "创建设备本地日程。title 必填；startsAt 与 delayMinutes 二选一。一分钟后必须传 delayMinutes=1，不能换算 startsAt。title 必须保留 PC/PZ 等人物。周期 startsAt 是从当前时刻起第一个尚未过去的实例。冲突确认必须等用户下一轮答复。",
        kCalendarCreateSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarCreate(arguments); });
    server.AddJsonTool("calendar_query",
        "仅用于‘要干什么/有什么安排/查日程’。不得先 calendar_find。rangeEnd 不包含；明确钟点必须只查该一分钟。",
        kCalendarQuerySchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarQuery(arguments); });
    server.AddJsonTool("calendar_find",
        "仅用于修改、跳过、暂停、恢复、终止或删除前定位，绝不能用于普通查询。用户给出日期或时间时必须同时传 rangeStart/rangeEnd；明确钟点只查该一分钟。多个候选不能猜测。",
        kCalendarFindSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarFind(arguments); });
    server.AddJsonTool("calendar_modify",
        "修改 calendar_find 返回的 eventId。单次日程 scope 可省略或为 this_occurrence；周期日程只支持 entire_series。冲突时先返回确认令牌。",
        kCalendarModifySchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarModify(arguments); });
    server.AddJsonTool("calendar_skip_occurrence",
        "跳过 calendar_find 返回的日程。周期事项传候选 originalStartAt；单次事项立即等同取消，不需要确认。",
        kEventIdSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarSkipOccurrence(arguments); });
    server.AddJsonTool("calendar_pause_series",
        "暂停本地周期日程。",
        kEventIdSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarPauseSeries(arguments); });
    server.AddJsonTool("calendar_resume_series",
        "恢复本地周期日程。",
        kEventIdSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarResumeSeries(arguments); });
    server.AddJsonTool("calendar_terminate_series",
        "终止本地日程后续周期。首次不传 confirmationToken；用户确认后原样回传该字段。",
        kConfirmEventSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarTerminateSeries(arguments); });
    server.AddJsonTool("calendar_delete",
        "删除 calendar_find 返回的日程。首次不传 confirmationToken；requiresConfirmation=true 后本轮必须停止，用户下一轮确认后才原样回传 confirmationToken。",
        kConfirmEventSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarDelete(arguments); });
    server.AddJsonTool("calendar_undo",
        "在十分钟内撤销最近一次本地写操作。",
        kUndoSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.CalendarUndo(arguments); });

    server.AddJsonTool("reminder_list_due",
        "列出设备本地已到期且未关闭的提醒。",
        R"json({"type":"object","properties":{}})json",
        [&service](const cJSON* arguments) -> ReturnValue { return service.ReminderListDue(arguments); });
    server.AddJsonTool("reminder_close",
        "关闭本地提醒，不修改日程。",
        kReminderIdSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.ReminderClose(arguments); });
    server.AddJsonTool("reminder_snooze",
        "推迟本地提醒 1 到 1440 分钟。",
        kSnoozeSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.ReminderSnooze(arguments); });
    server.AddJsonTool("reminder_get_details",
        "读取本地提醒的日程详情。",
        kReminderIdSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.ReminderGetDetails(arguments); });

    server.AddJsonTool("note_record",
        "保存 24 小时临时记录；密码、验证码和令牌会被拒绝。",
        kNoteRecordSchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.NoteRecord(arguments); });
    server.AddJsonTool("note_query",
        "查询仍在 24 小时有效期内的临时记录。",
        kNoteQuerySchema,
        [&service](const cJSON* arguments) -> ReturnValue { return service.NoteQuery(arguments); });
}

}  // namespace voicelife
