const eventIdSchema = {
  type: "object",
  properties: {
    eventId: { type: "string" },
    originalStartAt: { type: "string" },
  },
  required: ["eventId"],
};

const confirmEventSchema = {
  type: "object",
  properties: {
    eventId: { type: "string" },
    confirmationToken: { type: "string" },
  },
  required: ["eventId"],
};

const reminderIdSchema = {
  type: "object",
  properties: { reminderId: { type: "string" } },
  required: ["reminderId"],
};

export const tools = [
  {
    name: "calendar_create",
    description: "创建设备本地日程。title 必填；startsAt 与 delayMinutes 二选一。一分钟后必须传 delayMinutes=1，不能换算 startsAt。title 必须保留 PC/PZ 等人物。周期 startsAt 是从当前时刻起第一个尚未过去的实例。冲突确认必须等用户下一轮答复。",
    inputSchema: {
      type: "object",
      properties: {
        title: { type: "string" },
        startsAt: { type: "string", description: "ISO 8601 timestamp with timezone" },
        delayMinutes: { type: "integer", minimum: 1, maximum: 1440 },
        endsAt: { type: "string" },
        durationMinutes: { type: "integer", minimum: 1 },
        kind: { type: "string", enum: ["point", "time_block"] },
        remindAt: { type: "string" },
        weakReminder: { type: "boolean" },
        timeZone: { type: "string" },
        location: { type: "string" },
        notes: { type: "string" },
        recurrence: {
          type: "object",
          properties: {
            frequency: { type: "string", enum: ["daily", "weekly", "monthly"] },
            weekday: { type: "integer", minimum: 1, maximum: 7 },
            monthDay: { type: "integer", minimum: 1, maximum: 31 },
          },
          required: ["frequency"],
        },
        conflictConfirmationToken: { type: "string" },
      },
      required: ["title"],
    },
  },
  {
    name: "calendar_query",
    description: "仅用于‘要干什么/有什么安排/查日程’。不得先 calendar_find。rangeEnd 不包含；明确钟点必须只查该一分钟。",
    inputSchema: {
      type: "object",
      properties: { rangeStart: { type: "string" }, rangeEnd: { type: "string" } },
      required: ["rangeStart", "rangeEnd"],
    },
  },
  {
    name: "calendar_find",
    description: "仅用于修改、跳过、暂停、恢复、终止或删除前定位，绝不能用于普通查询。用户给出日期或时间时必须同时传 rangeStart/rangeEnd；明确钟点只查该一分钟。多个候选不能猜测。",
    inputSchema: {
      type: "object",
      properties: {
        query: { type: "string" },
        rangeStart: { type: "string" },
        rangeEnd: { type: "string" },
      },
      required: ["query"],
    },
  },
  {
    name: "calendar_modify",
    description: "修改 calendar_find 返回的 eventId。单次日程 scope 可省略或为 this_occurrence；周期日程只支持 entire_series。冲突时先返回确认令牌。",
    inputSchema: {
      type: "object",
      properties: {
        eventId: { type: "string" },
        scope: { type: "string", enum: ["this_occurrence", "this_and_future", "entire_series"] },
        originalStartAt: { type: "string" },
        newStartAt: { type: "string" },
        startsAt: { type: "string" },
        endsAt: { type: "string" },
        title: { type: "string" },
        location: { type: "string" },
        notes: { type: "string" },
        weakReminder: { type: "boolean" },
        conflictConfirmationToken: { type: "string" },
      },
      required: ["eventId"],
    },
  },
  { name: "calendar_skip_occurrence", description: "跳过 calendar_find 返回的日程。周期事项传候选 originalStartAt；单次事项立即等同取消，不需要确认。", inputSchema: eventIdSchema },
  { name: "calendar_pause_series", description: "暂停本地周期日程。", inputSchema: eventIdSchema },
  { name: "calendar_resume_series", description: "恢复本地周期日程。", inputSchema: eventIdSchema },
  { name: "calendar_terminate_series", description: "终止本地日程后续周期。首次不传 confirmationToken；用户确认后原样回传该字段。", inputSchema: confirmEventSchema },
  { name: "calendar_delete", description: "删除 calendar_find 返回的日程。首次不传 confirmationToken；requiresConfirmation=true 后本轮必须停止，用户下一轮确认后才原样回传 confirmationToken。", inputSchema: confirmEventSchema },
  {
    name: "calendar_undo",
    description: "在十分钟内撤销最近一次本地写操作。",
    inputSchema: {
      type: "object",
      properties: { undoOperationId: { type: "string" } },
      required: ["undoOperationId"],
    },
  },
  { name: "reminder_list_due", description: "列出设备本地到期提醒。", inputSchema: { type: "object", properties: {} } },
  { name: "reminder_close", description: "关闭本地提醒。", inputSchema: reminderIdSchema },
  {
    name: "reminder_snooze",
    description: "推迟本地提醒 1 到 1440 分钟。",
    inputSchema: {
      type: "object",
      properties: {
        reminderId: { type: "string" },
        minutes: { type: "integer", minimum: 1, maximum: 1440 },
      },
      required: ["reminderId", "minutes"],
    },
  },
  { name: "reminder_get_details", description: "读取本地提醒详情。", inputSchema: reminderIdSchema },
  {
    name: "note_record",
    description: "保存 24 小时临时记录；密码、验证码和令牌会被拒绝。",
    inputSchema: {
      type: "object",
      properties: { content: { type: "string" }, category: { type: "string" } },
      required: ["content"],
    },
  },
  {
    name: "note_query",
    description: "查询仍在 24 小时有效期内的临时记录。",
    inputSchema: { type: "object", properties: { query: { type: "string" } } },
  },
];

export function protocolToolResult(call) {
  if (call.name !== "calendar_create") {
    return { ok: false, message: `Protocol smoke does not execute ${call.name}` };
  }
  return {
    ok: true,
    eventId: "protocol-smoke-event",
    reminderId: "protocol-smoke-reminder",
    undoOperationId: "protocol-smoke-undo",
    speech: `已创建${call.arguments.title || "开会"}，时间是${call.arguments.startsAt}`,
  };
}
