import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import type { CalendarOccurrence } from "../domain/types.js";
import { formatChineseDateTime } from "../domain/recurrence.js";
import {
  CalendarConflictError,
  CalendarService,
} from "../services/calendar-service.js";
import {
  CalendarConfirmationRequiredError,
  CalendarMutationService,
} from "../services/calendar-mutation-service.js";
import { ReminderService } from "../services/reminder-service.js";
import { ShortNoteService } from "../services/short-note-service.js";

function result(payload: Record<string, unknown>) {
  return {
    content: [{ type: "text" as const, text: JSON.stringify(payload) }],
    structuredContent: payload,
  };
}

function failure(error: unknown) {
  const message = error instanceof Error ? error.message : "未知错误";
  return {
    content: [{ type: "text" as const, text: JSON.stringify({ ok: false, message }) }],
    structuredContent: { ok: false, message },
    isError: true,
  };
}

function confirmationRequiredResult(error: CalendarConfirmationRequiredError) {
  return result({
    ok: false,
    reason: error.action,
    speech: error.message,
    requiresConfirmation: true,
    confirmationToken: error.confirmationToken,
    conflicts: error.conflicts,
  });
}

function summarizeOccurrence(item: CalendarOccurrence): string {
  return `${formatChineseDateTime(item.effectiveStartAt, item.timeZone)} ${item.title}`;
}

export function createCalendarMcpServer(
  calendarService: CalendarService,
  reminderService: ReminderService,
  mutationService: CalendarMutationService,
  shortNoteService: ShortNoteService,
): McpServer {
  const server = new McpServer({
    name: "voice-calendar-prototype",
    version: "0.1.0",
  });

  server.registerTool(
    "calendar_create",
    {
      title: "创建日程",
      description:
        "创建单次或每天/每周/每月日程。会议、拜访、课程等占用时间段的事项必须先取得 endsAt 或 durationMinutes；普通时间点提醒只需 startsAt。时间段日程默认在开始前 15 分钟生成一条无需回应的弱提醒；只有用户明确说不需要提前提醒时才传 weakReminder=false。服务端会检查时间段重叠及同一时刻的点提醒冲突。首次调用省略 conflictConfirmationToken；冲突后必须询问用户，确认仍然创建时原样传回令牌。remindAt 不填时等于 startsAt。",
      inputSchema: {
        title: z.string().min(1).describe("日程标题"),
        startsAt: z.string().describe("带时区的 ISO 8601 发生时间"),
        endsAt: z.string().optional().describe("时间段日程的 ISO 8601 结束时间"),
        durationMinutes: z.number().int().min(1).optional().describe("时间段日程持续分钟数，与 endsAt 二选一"),
        kind: z.enum(["point", "time_block"]).optional().describe("时间点提醒或占用时间段的日程"),
        remindAt: z.string().optional().describe("带时区的 ISO 8601 提醒时间"),
        weakReminder: z.boolean().optional().describe("时间段日程默认 true；仅当用户明确拒绝提前提醒时传 false"),
        weakReminderMinutes: z.literal(15).optional().describe("兼容字段：显式为时间点日程开启提前 15 分钟弱提醒"),
        location: z.string().optional().describe("地点"),
        notes: z.string().optional().describe("备注"),
        recurrence: z
          .object({
            frequency: z.enum(["daily", "weekly", "monthly"]),
            weekday: z.number().int().min(1).max(7).optional(),
            monthDay: z.number().int().min(1).max(31).optional(),
          })
          .optional()
          .describe("只支持每天、每周、每月"),
        conflictConfirmationToken: z
          .string()
          .optional()
          .describe("仅在上一次调用返回时间冲突且用户明确确认仍要创建同一日程后，原样传回响应中的确认令牌；首次调用必须省略"),
      },
    },
    async (input) => {
      try {
        const created = calendarService.create(input);
        return result({
          ok: true,
          speech: created.conflicts.length
            ? `已按你的确认创建${created.event.title}，时间是${formatChineseDateTime(created.event.startAt, created.event.timeZone)}，并保留了与${created.conflicts.map((item) => item.title).join("、")}的时间冲突。`
            : `已创建${created.event.title}，时间是${formatChineseDateTime(created.event.startAt, created.event.timeZone)}`,
          eventId: created.event.id,
          reminderId: created.reminder.id,
          startsAt: created.event.startAt,
          endsAt: created.event.endAt,
          kind: created.event.kind,
          remindAt: created.reminder.triggerAt,
          weakRemindAt: created.weakReminder?.triggerAt ?? null,
          weakReminderEnabled: created.event.weakReminderEnabled,
          recurrence: created.event.recurrenceFrequency,
          nextOccurrenceAt: created.nextOccurrenceAt,
          conflictConfirmed: created.conflicts.length > 0,
          receiptId: created.receipt.id,
        });
      } catch (error) {
        if (error instanceof CalendarConflictError) {
          const conflictTitles = error.conflicts.map((item) => `“${item.title}”`).join("、");
          return result({
            ok: false,
            reason: "calendar_conflict",
            speech: `${formatChineseDateTime(error.requestedStartAt, error.conflicts[0]!.timeZone)}已经有${conflictTitles}，与“${error.requestedTitle}”时间冲突。仍然要创建吗？`,
            requestedTitle: error.requestedTitle,
            requestedStartAt: error.requestedStartAt,
            requestedEndAt: error.requestedEndAt,
            conflicts: error.conflicts,
            conflictConfirmationToken: error.confirmationToken,
            requiresConfirmation: true,
          });
        }
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_query",
    {
      title: "查询日程",
      description:
        "按用户原话对应的明确时间范围查询今天、指定日期、本周、本月或今年。自然日查询必须使用当地日期边界：‘今天’是今天 00:00（包含）到明天 00:00（不包含），‘明天’是明天 00:00 到后天 00:00；即使日程时间已过也保留在该自然日结果中。只有‘接下来’‘之后’‘未来’等未来语义才从当前时间开始；禁止把‘今天’查询成现在起未来 24 小时。语音只播报前两条，完整列表已同时写入 IM 回执。",
      inputSchema: {
        rangeStart: z
          .string()
          .describe("带时区的 ISO 8601 查询开始时间，包含；查询今天或明天时必须是对应自然日的 00:00，只有未来语义才使用当前时间"),
        rangeEnd: z
          .string()
          .describe("带时区的 ISO 8601 查询结束时间，不包含；自然日查询必须是下一自然日的 00:00，不得使用当前时间加 24 小时"),
      },
    },
    async (input) => {
      try {
        const queried = calendarService.query(input);
        const summaries = queried.occurrences.slice(0, 2).map(summarizeOccurrence);
        const speech = queried.occurrences.length
          ? `共有${queried.occurrences.length}条安排。${summaries.join("；")}`
          : "这个时间范围内暂时没有安排。";
        return result({
          ok: true,
          speech,
          total: queried.occurrences.length,
          occurrences: queried.occurrences,
          receiptId: queried.receipt.id,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_find",
    {
      title: "查找待修改日程",
      description:
        "修改日程前按标题和可选时间范围查找候选项。返回多个候选时必须让用户指定，不能猜测。",
      inputSchema: {
        query: z.string().min(1).describe("日程标题关键词"),
        rangeStart: z.string().optional().describe("ISO 8601 范围开始"),
        rangeEnd: z.string().optional().describe("ISO 8601 范围结束"),
      },
    },
    async (input) => {
      try {
        const candidates = calendarService.find(input);
        return result({
          ok: true,
          speech:
            candidates.length === 0
              ? "没有找到符合条件的日程。"
              : candidates.length === 1
                ? `找到${summarizeOccurrence(candidates[0]!)}`
                : `找到${candidates.length}条日程，请用户指定要修改哪一条。`,
          candidates,
          requiresDisambiguation: candidates.length > 1,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_reschedule_occurrence",
    {
      title: "仅修改一次周期日程",
      description:
        "兼容入口：只修改指定周期实例的时间，其他周期不变。新时间冲突时必须向用户确认，再原样传回 conflictConfirmationToken。eventId 和 originalStartAt 必须来自 calendar_find 或 calendar_query。",
      inputSchema: {
        eventId: z.string().uuid(),
        originalStartAt: z.string().describe("原周期实例的 ISO 8601 时间"),
        newStartAt: z.string().describe("新的 ISO 8601 时间"),
        conflictConfirmationToken: z.string().optional(),
      },
    },
    async (input) => {
      try {
        const updated = mutationService.modify({ ...input, scope: "this_occurrence" });
        return result({
          ok: true,
          speech: `已把${updated.occurrence.title}这一次改到${formatChineseDateTime(updated.occurrence.effectiveStartAt, updated.occurrence.timeZone)}，后续周期不变。`,
          occurrence: updated.occurrence,
          receiptId: updated.receipt.id,
          undoOperationId: updated.undoOperation.id,
        });
      } catch (error) {
        if (error instanceof CalendarConfirmationRequiredError) {
          return confirmationRequiredResult(error);
        }
        return failure(error);
      }
    },
  );

  server.registerTool(
    "reminder_list_due",
    {
      title: "查询到期提醒",
      description:
        "当用户问有什么到期提醒时调用。返回多个提醒时只列出候选并要求用户指定。",
    },
    async () => {
      try {
        await reminderService.scanDue();
        const due = reminderService.listDue();
        return result({
          ok: true,
          speech:
            due.length === 0
              ? "现在没有到期提醒。"
              : due.length === 1
                ? `提醒：${summarizeOccurrence(due[0]!.occurrence)}`
                : `现在有${due.length}条到期提醒：${due.map((item) => item.occurrence.title).join("、")}。请指定要处理哪一条。`,
          reminders: due.map(({ reminder, occurrence }) => ({
            reminderId: reminder.id,
            title: occurrence.title,
            effectiveStartAt: occurrence.effectiveStartAt,
            snoozeCount: reminder.snoozeCount,
            location: occurrence.location,
            notes: occurrence.notes,
          })),
          requiresDisambiguation: due.length > 1,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "reminder_close",
    {
      title: "关闭当前提醒",
      description: "用户说知道了或不用再响了时调用。只关闭提醒，不修改日程。",
      inputSchema: {
        reminderId: z.string().uuid().describe("来自 reminder_list_due 的提醒 ID"),
      },
    },
    async ({ reminderId }) => {
      try {
        const closed = reminderService.close(reminderId);
        return result({
          ok: true,
          speech: closed.alreadyClosed ? "这条提醒已经关闭。" : "好的，已关闭提醒，日程保持不变。",
          reminderId,
          alreadyClosed: closed.alreadyClosed,
          receiptId: closed.receipt?.id ?? null,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "reminder_snooze",
    {
      title: "推迟当前提醒",
      description:
        "用户明确说出推迟分钟数后调用。未说明多久时必须先追问。只推迟提醒，不修改日程。",
      inputSchema: {
        reminderId: z.string().uuid().describe("来自 reminder_list_due 的提醒 ID"),
        minutes: z.number().int().min(1).max(1440),
      },
    },
    async ({ reminderId, minutes }) => {
      try {
        const snoozed = reminderService.snooze(reminderId, minutes);
        return result({
          ok: true,
          speech: snoozed.alreadySnoozed
            ? "这条提醒已经推迟过了。"
            : `好的，${minutes}分钟后再次提醒，日程保持不变。`,
          reminderId,
          nextTriggerAt: snoozed.reminder.triggerAt,
          snoozeCount: snoozed.reminder.snoozeCount,
          alreadySnoozed: snoozed.alreadySnoozed,
          receiptId: snoozed.receipt?.id ?? null,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "reminder_get_details",
    {
      title: "获取提醒详情",
      description: "用户对某条到期提醒说再详细说说时调用。",
      inputSchema: {
        reminderId: z.string().uuid(),
      },
    },
    async ({ reminderId }) => {
      try {
        const details = reminderService.getDetails(reminderId);
        const { occurrence } = details;
        return result({
          ok: true,
          speech: `${occurrence.title}，时间是${formatChineseDateTime(occurrence.effectiveStartAt, occurrence.timeZone)}${occurrence.location ? `，地点${occurrence.location}` : ""}${occurrence.notes ? `，备注${occurrence.notes}` : ""}。`,
          reminderId,
          occurrence,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "note_record",
    {
      title: "记录临时小事",
      description:
        "记录无明确提醒时间的非敏感小事，例如停车位置或刚才说过的事项，24 小时后自动过期。若用户给了提醒时间，应改用 calendar_create。密码、验证码、取件码等敏感内容不得记录。",
      inputSchema: {
        content: z.string().min(1),
        category: z.string().optional().describe("可选分类，例如 parking"),
      },
    },
    async (input) => {
      try {
        const recorded = shortNoteService.record(input);
        return result({
          ok: true,
          speech: `记住了：${recorded.note.content}。这条临时记录保留二十四小时。`,
          note: recorded.note,
          receiptId: recorded.receipt.id,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "note_query",
    {
      title: "查询临时小事",
      description: "查询仍在 24 小时有效期内的临时记录，例如‘我车停哪了’或‘我刚才记了什么’。",
      inputSchema: {
        query: z.string().optional().describe("可选关键词；询问刚才记录了什么时可省略"),
      },
    },
    async ({ query }) => {
      try {
        const queried = shortNoteService.query(query);
        return result({
          ok: true,
          speech: queried.notes.length
            ? queried.notes.slice(0, 2).map((note) => note.content).join("；")
            : "没有找到仍在有效期内的临时记录。",
          total: queried.notes.length,
          notes: queried.notes,
          receiptId: queried.receipt.id,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_modify",
    {
      title: "修改日程",
      description:
        "修改单次日程或周期日程的标题、时间、结束时间、地点、备注。周期日程必须从用户明确取得 scope：this_occurrence（仅本次）、this_and_future（本次及以后）或 entire_series（整个系列）；不能猜测。若返回冲突确认，必须询问后原样传回令牌。",
      inputSchema: {
        eventId: z.string().uuid(),
        originalStartAt: z.string().describe("来自 calendar_find/query 的原实例时间"),
        scope: z.enum(["this_occurrence", "this_and_future", "entire_series"]),
        title: z.string().min(1).optional(),
        newStartAt: z.string().optional(),
        endsAt: z.string().nullable().optional(),
        location: z.string().nullable().optional(),
        notes: z.string().nullable().optional(),
        conflictConfirmationToken: z.string().optional(),
      },
    },
    async (input) => {
      try {
        const changed = mutationService.modify(input);
        return result({
          ok: true,
          speech: `已修改${changed.occurrence.title}，时间是${formatChineseDateTime(changed.occurrence.effectiveStartAt, changed.occurrence.timeZone)}。`,
          occurrence: changed.occurrence,
          receiptId: changed.receipt.id,
          undoOperationId: changed.undoOperation.id,
          undoExpiresInMinutes: 10,
        });
      } catch (error) {
        if (error instanceof CalendarConfirmationRequiredError) return confirmationRequiredResult(error);
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_skip_occurrence",
    {
      title: "跳过本次日程",
      description:
        "跳过一次周期实例。若目标是单次日程，会解释其等同取消。属于高风险操作：首次省略 confirmationToken，向用户说明结果；用户确认后原样传回令牌。重复跳过保持幂等。",
      inputSchema: {
        eventId: z.string().uuid(),
        originalStartAt: z.string(),
        confirmationToken: z.string().optional(),
      },
    },
    async (input) => {
      try {
        const skipped = mutationService.skip(input);
        return result({
          ok: true,
          speech: skipped.alreadySkipped ? "这一次已经跳过了。" : "已跳过这一次，其他周期不变。",
          alreadySkipped: skipped.alreadySkipped,
          receiptId: skipped.receipt?.id ?? null,
          undoOperationId: skipped.undoOperation?.id ?? null,
        });
      } catch (error) {
        if (error instanceof CalendarConfirmationRequiredError) return confirmationRequiredResult(error);
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_pause_series",
    {
      title: "暂停周期日程",
      description: "暂停周期日程到指定时间。必须向用户取得明确的恢复日期；暂停期间的实例不会查询或提醒。",
      inputSchema: {
        eventId: z.string().uuid(),
        from: z.string().optional().describe("暂停开始时间，省略时为当前时刻"),
        until: z.string().describe("暂停截止 ISO 8601 时间"),
      },
    },
    async (input) => {
      try {
        const paused = mutationService.pause(input);
        return result({
          ok: true,
          speech: paused.receipt.body,
          receiptId: paused.receipt.id,
          undoOperationId: paused.undoOperation.id,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_resume_series",
    {
      title: "恢复周期日程",
      description: "提前恢复当前处于暂停状态的周期日程。",
      inputSchema: { eventId: z.string().uuid() },
    },
    async ({ eventId }) => {
      try {
        const resumed = mutationService.resume(eventId);
        return result({
          ok: true,
          speech: `已恢复${resumed.receipt.body}。`,
          receiptId: resumed.receipt.id,
          undoOperationId: resumed.undoOperation.id,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_terminate_series",
    {
      title: "终止周期日程",
      description: "从指定实例起终止全部未来周期。高风险操作，必须先取得用户确认并回传确认令牌。",
      inputSchema: {
        eventId: z.string().uuid(),
        from: z.string(),
        confirmationToken: z.string().optional(),
      },
    },
    async (input) => {
      try {
        const terminated = mutationService.terminate(input);
        return result({
          ok: true,
          speech: terminated.receipt.body,
          receiptId: terminated.receipt.id,
          undoOperationId: terminated.undoOperation.id,
        });
      } catch (error) {
        if (error instanceof CalendarConfirmationRequiredError) return confirmationRequiredResult(error);
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_delete",
    {
      title: "删除日程",
      description: "删除单次日程或整个周期系列。高风险操作，必须先取得用户确认并回传确认令牌。",
      inputSchema: {
        eventId: z.string().uuid(),
        confirmationToken: z.string().optional(),
      },
    },
    async (input) => {
      try {
        const deleted = mutationService.delete(input);
        return result({
          ok: true,
          speech: deleted.receipt.body,
          receiptId: deleted.receipt.id,
          undoOperationId: deleted.undoOperation.id,
        });
      } catch (error) {
        if (error instanceof CalendarConfirmationRequiredError) return confirmationRequiredResult(error);
        return failure(error);
      }
    },
  );

  server.registerTool(
    "calendar_undo",
    {
      title: "撤销近期日程操作",
      description: "撤销最近 10 分钟内的一次修改、跳过、暂停、恢复、终止或删除操作。",
      inputSchema: {
        operationId: z.string().uuid().optional().describe("来自操作回执；省略时撤销最近一次可撤销操作"),
      },
    },
    async ({ operationId }) => {
      try {
        const undone = mutationService.undo(operationId);
        return result({
          ok: true,
          speech: undone.receipt.body,
          operationId: undone.operation.id,
          receiptId: undone.receipt.id,
        });
      } catch (error) {
        return failure(error);
      }
    },
  );

  return server;
}
