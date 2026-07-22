import { createHash, randomUUID } from "node:crypto";
import type {
  CalendarEvent,
  CalendarOccurrence,
  OccurrenceOverride,
  Receipt,
  RecurrenceScope,
  ReminderInstance,
  UndoOperation,
  WeakReminderInstance,
} from "../domain/types.js";
import { formatChineseDateTime, parseDateTime, toStorageIso } from "../domain/recurrence.js";
import { CalendarDatabase } from "../storage/database.js";
import { CalendarService } from "./calendar-service.js";
import type { Clock } from "./clock.js";
import { ReceiptBus } from "./receipt-bus.js";

interface EventSnapshot {
  event: CalendarEvent;
  overrides: OccurrenceOverride[];
  reminders: ReminderInstance[];
  weakReminders: WeakReminderInstance[];
}

export class CalendarConfirmationRequiredError extends Error {
  public constructor(
    message: string,
    public readonly action: string,
    public readonly confirmationToken: string,
    public readonly conflicts: CalendarOccurrence[] = [],
  ) {
    super(message);
    this.name = "CalendarConfirmationRequiredError";
  }
}

export interface ModifyCalendarInput {
  eventId: string;
  originalStartAt: string;
  scope: RecurrenceScope;
  title?: string;
  newStartAt?: string;
  endsAt?: string | null;
  location?: string | null;
  notes?: string | null;
  conflictConfirmationToken?: string;
}

export class CalendarMutationService {
  public constructor(
    private readonly db: CalendarDatabase,
    private readonly calendarService: CalendarService,
    private readonly clock: Clock,
    private readonly receiptBus: ReceiptBus,
    private readonly timeZone: string,
  ) {}

  modify(input: ModifyCalendarInput): {
    occurrence: CalendarOccurrence;
    receipt: Receipt;
    undoOperation: UndoOperation;
  } {
    const event = this.requireEvent(input.eventId);
    const originalStartAt = toStorageIso(parseDateTime(input.originalStartAt, event.timeZone));
    const occurrence = this.calendarService.getOccurrence(event.id, originalStartAt);
    if (!event.recurrenceFrequency && input.scope !== "this_occurrence") {
      throw new Error("单次日程只能按本次修改");
    }
    if (
      input.title === undefined && input.newStartAt === undefined && input.endsAt === undefined &&
      input.location === undefined && input.notes === undefined
    ) {
      throw new Error("没有提供需要修改的字段");
    }

    const newStartAt = input.newStartAt
      ? toStorageIso(parseDateTime(input.newStartAt, event.timeZone))
      : occurrence.effectiveStartAt;
    if (parseDateTime(newStartAt, event.timeZone) < this.clock.now().minus({ seconds: 1 })) {
      throw new Error("新的日程时间已经过去");
    }
    const newEndAt = this.resolveEndAt(input, occurrence, newStartAt);
    if (event.kind === "point" && newEndAt) {
      throw new Error("时间点提醒不能设置结束时间");
    }
    const scheduleChanged = input.newStartAt !== undefined || input.endsAt !== undefined;
    const conflicts = scheduleChanged
      ? this.calendarService.findConflictsForChange(
          newStartAt,
          newEndAt,
          event.kind,
          event.id,
        )
      : [];
    if (conflicts.length > 0) {
      const payload = {
        eventId: event.id,
        originalStartAt,
        scope: input.scope,
        newStartAt,
        newEndAt,
        conflictIds: conflicts.map((item) => `${item.eventId}:${item.originalStartAt}`).sort(),
      };
      this.requireConfirmation(
        "modify_with_conflict",
        payload,
        input.conflictConfirmationToken,
        "修改后的时间与现有日程冲突，需要用户确认后才能保存",
        conflicts,
      );
    }

    const now = toStorageIso(this.clock.now());
    const snapshot = this.captureEvent(event.id);
    const splitEventId = event.recurrenceFrequency && input.scope === "this_and_future"
      ? randomUUID()
      : null;
    const effectiveEventId = splitEventId ?? event.id;
    const resultOriginalStartAt = splitEventId || (!event.recurrenceFrequency || input.scope === "entire_series")
      ? (input.newStartAt === undefined && !splitEventId ? event.startAt : newStartAt)
      : originalStartAt;
    const removeEventIds = splitEventId ? [splitEventId] : [];

    const mutation = () => {
      if (event.recurrenceFrequency && input.scope === "this_occurrence") {
        const existing = this.db.getOverride(event.id, originalStartAt);
        const override: OccurrenceOverride = {
          id: existing?.id ?? randomUUID(),
          eventId: event.id,
          originalStartAt,
          newStartAt: input.newStartAt === undefined ? existing?.newStartAt ?? null : newStartAt,
          patch: {
            ...existing?.patch,
            ...(input.title === undefined ? {} : { title: this.requiredText(input.title, "标题") }),
            ...(input.endsAt === undefined ? {} : { endAt: newEndAt }),
            ...(input.location === undefined ? {} : { location: this.optionalText(input.location) }),
            ...(input.notes === undefined ? {} : { notes: this.optionalText(input.notes) }),
          },
          status: "moved",
          createdAt: existing?.createdAt ?? now,
          updatedAt: now,
        };
        this.db.upsertOverride(override);
        this.replaceOccurrenceReminders(event, originalStartAt, newStartAt, now);
        return;
      }

      if (event.recurrenceFrequency && input.scope === "this_and_future") {
        const oldEvent: CalendarEvent = { ...event, terminatedAt: originalStartAt, updatedAt: now };
        const newEvent: CalendarEvent = {
          ...event,
          id: splitEventId!,
          title: input.title === undefined ? occurrence.title : this.requiredText(input.title, "标题"),
          startAt: newStartAt,
          endAt: newEndAt,
          location: input.location === undefined ? occurrence.location : this.optionalText(input.location),
          notes: input.notes === undefined ? occurrence.notes : this.optionalText(input.notes),
          recurrenceWeekday: event.recurrenceFrequency === "weekly"
            ? parseDateTime(newStartAt, event.timeZone).weekday
            : null,
          recurrenceMonthDay: event.recurrenceFrequency === "monthly"
            ? parseDateTime(newStartAt, event.timeZone).day
            : null,
          pausedFrom: null,
          pausedUntil: null,
          terminatedAt: null,
          createdAt: now,
          updatedAt: now,
        };
        this.db.upsertEvent(oldEvent);
        this.db.deleteRemindersFrom(oldEvent.id, originalStartAt);
        this.db.deleteWeakRemindersFrom(oldEvent.id, originalStartAt);
        this.db.insertEvent(newEvent);
        this.insertInitialReminders(newEvent, now);
        return;
      }

      const updated: CalendarEvent = {
        ...event,
        title: input.title === undefined ? event.title : this.requiredText(input.title, "标题"),
        startAt: input.newStartAt === undefined ? event.startAt : newStartAt,
        endAt: input.endsAt === undefined && input.newStartAt === undefined ? event.endAt : newEndAt,
        location: input.location === undefined ? event.location : this.optionalText(input.location),
        notes: input.notes === undefined ? event.notes : this.optionalText(input.notes),
        recurrenceWeekday: event.recurrenceFrequency === "weekly" && input.newStartAt !== undefined
          ? parseDateTime(newStartAt, event.timeZone).weekday
          : event.recurrenceWeekday,
        recurrenceMonthDay: event.recurrenceFrequency === "monthly" && input.newStartAt !== undefined
          ? parseDateTime(newStartAt, event.timeZone).day
          : event.recurrenceMonthDay,
        updatedAt: now,
      };
      this.db.upsertEvent(updated);
      if (input.newStartAt !== undefined) this.db.deleteOverrides(event.id);
      this.db.deleteRemindersFrom(event.id, "");
      this.db.deleteWeakRemindersFrom(event.id, "");
      this.insertInitialReminders(updated, now);
    };

    const scopeText = input.scope === "this_occurrence"
      ? "仅本次"
      : input.scope === "this_and_future"
        ? "本次及以后"
        : "整个系列";
    const undoOperation = this.buildUndo(
      "modify",
      `撤销“${event.title}”的${scopeText}修改`,
      [snapshot],
      removeEventIds,
      now,
    );
    const receipt = this.receipt({
      type: "calendar_modified",
      eventId: effectiveEventId,
      title: "已修改日程",
      body: `${event.title} · ${scopeText} · ${formatChineseDateTime(newStartAt, event.timeZone)}`,
      data: { scope: input.scope, originalStartAt, newStartAt, newEndAt, undoOperationId: undoOperation.id },
      createdAt: now,
    });
    this.db.transaction(() => {
      mutation();
      this.db.insertUndoOperation(undoOperation);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return {
      occurrence: this.calendarService.getOccurrence(effectiveEventId, resultOriginalStartAt),
      receipt,
      undoOperation,
    };
  }

  skip(input: {
    eventId: string;
    originalStartAt: string;
    confirmationToken?: string;
  }): { alreadySkipped: boolean; receipt: Receipt | null; undoOperation: UndoOperation | null } {
    const event = this.requireEvent(input.eventId);
    const originalStartAt = toStorageIso(parseDateTime(input.originalStartAt, event.timeZone));
    const occurrence = this.calendarService.getOccurrence(event.id, originalStartAt);
    const action = event.recurrenceFrequency ? "skip_occurrence" : "cancel_one_off";
    if (event.recurrenceFrequency && this.db.getOverride(event.id, originalStartAt)?.status === "skipped") {
      return { alreadySkipped: true, receipt: null, undoOperation: null };
    }
    this.requireConfirmation(
      action,
      { eventId: event.id, originalStartAt },
      input.confirmationToken,
      event.recurrenceFrequency
        ? "跳过本次后，本次日程和提醒将不再出现。确认跳过吗？"
        : "单次日程没有下一次；这里的跳过等同取消。确认取消吗？",
    );
    const now = toStorageIso(this.clock.now());
    const snapshot = this.captureEvent(event.id);
    const undoOperation = this.buildUndo(action, `撤销“${occurrence.title}”的跳过或取消`, [snapshot], [], now);
    const receipt = this.receipt({
      type: event.recurrenceFrequency ? "calendar_skipped" : "calendar_deleted",
      eventId: event.recurrenceFrequency ? event.id : null,
      title: event.recurrenceFrequency ? "已跳过本次日程" : "已取消单次日程",
      body: `${occurrence.title} · ${formatChineseDateTime(occurrence.effectiveStartAt, occurrence.timeZone)}`,
      data: { originalStartAt, scope: "this_occurrence", undoOperationId: undoOperation.id },
      createdAt: now,
    });
    this.db.transaction(() => {
      if (!event.recurrenceFrequency) {
        this.db.deleteEvent(event.id);
      } else {
        const existing = this.db.getOverride(event.id, originalStartAt);
        this.db.upsertOverride({
          id: existing?.id ?? randomUUID(),
          eventId: event.id,
          originalStartAt,
          newStartAt: null,
          patch: existing?.patch ?? {},
          status: "skipped",
          createdAt: existing?.createdAt ?? now,
          updatedAt: now,
        });
        this.db.deleteReminderByOccurrence(event.id, originalStartAt);
        this.db.deleteWeakReminderByOccurrence(event.id, originalStartAt);
      }
      this.db.insertUndoOperation(undoOperation);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return { alreadySkipped: false, receipt, undoOperation };
  }

  pause(input: { eventId: string; until: string; from?: string }): { receipt: Receipt; undoOperation: UndoOperation } {
    const event = this.requireRecurringEvent(input.eventId);
    const from = input.from
      ? toStorageIso(parseDateTime(input.from, event.timeZone))
      : toStorageIso(this.clock.now());
    const until = toStorageIso(parseDateTime(input.until, event.timeZone));
    if (until <= from) throw new Error("暂停截止时间必须晚于暂停开始时间");
    const now = toStorageIso(this.clock.now());
    const undoOperation = this.buildUndo("pause", `撤销“${event.title}”的暂停`, [this.captureEvent(event.id)], [], now);
    const receipt = this.receipt({
      type: "calendar_paused",
      eventId: event.id,
      title: "已暂停周期日程",
      body: `${event.title} · 暂停至 ${formatChineseDateTime(until, event.timeZone)}`,
      data: { from, until, undoOperationId: undoOperation.id },
      createdAt: now,
    });
    this.db.transaction(() => {
      this.db.upsertEvent({ ...event, pausedFrom: from, pausedUntil: until, updatedAt: now });
      this.db.deleteRemindersFrom(event.id, from);
      this.db.deleteWeakRemindersFrom(event.id, from);
      this.db.insertUndoOperation(undoOperation);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return { receipt, undoOperation };
  }

  resume(eventId: string): { receipt: Receipt; undoOperation: UndoOperation } {
    const event = this.requireRecurringEvent(eventId);
    if (!event.pausedUntil) throw new Error("该周期日程当前没有暂停");
    const now = toStorageIso(this.clock.now());
    const undoOperation = this.buildUndo("resume", `撤销“${event.title}”的恢复`, [this.captureEvent(event.id)], [], now);
    const updated = { ...event, pausedFrom: null, pausedUntil: null, updatedAt: now };
    const receipt = this.receipt({
      type: "calendar_resumed",
      eventId: event.id,
      title: "已恢复周期日程",
      body: event.title,
      data: { undoOperationId: undoOperation.id },
      createdAt: now,
    });
    this.db.transaction(() => {
      this.db.upsertEvent(updated);
      this.insertNextAvailableReminder(updated, now);
      this.db.insertUndoOperation(undoOperation);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return { receipt, undoOperation };
  }

  terminate(input: { eventId: string; from: string; confirmationToken?: string }): { receipt: Receipt; undoOperation: UndoOperation } {
    const event = this.requireRecurringEvent(input.eventId);
    const from = toStorageIso(parseDateTime(input.from, event.timeZone));
    this.calendarService.getOccurrence(event.id, from);
    this.requireConfirmation(
      "terminate_series",
      { eventId: event.id, from },
      input.confirmationToken,
      "终止后该时间起的所有未来日程都不会再发生。确认终止吗？",
    );
    const now = toStorageIso(this.clock.now());
    const undoOperation = this.buildUndo("terminate", `撤销“${event.title}”的终止`, [this.captureEvent(event.id)], [], now);
    const receipt = this.receipt({
      type: "calendar_terminated",
      eventId: event.id,
      title: "已终止周期日程",
      body: `${event.title} · ${formatChineseDateTime(from, event.timeZone)}起不再发生`,
      data: { from, undoOperationId: undoOperation.id },
      createdAt: now,
    });
    this.db.transaction(() => {
      this.db.upsertEvent({ ...event, terminatedAt: from, updatedAt: now });
      this.db.deleteRemindersFrom(event.id, from);
      this.db.deleteWeakRemindersFrom(event.id, from);
      this.db.insertUndoOperation(undoOperation);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return { receipt, undoOperation };
  }

  delete(input: { eventId: string; confirmationToken?: string }): { receipt: Receipt; undoOperation: UndoOperation } {
    const event = this.requireEvent(input.eventId);
    this.requireConfirmation(
      "delete_event",
      { eventId: event.id },
      input.confirmationToken,
      event.recurrenceFrequency ? "删除后整个系列及其提醒都会消失。确认删除吗？" : "确认删除这条日程吗？",
    );
    const now = toStorageIso(this.clock.now());
    const undoOperation = this.buildUndo("delete", `撤销删除“${event.title}”`, [this.captureEvent(event.id)], [], now);
    const receipt = this.receipt({
      type: "calendar_deleted",
      eventId: null,
      title: "已删除日程",
      body: event.title,
      data: { eventId: event.id, entireSeries: Boolean(event.recurrenceFrequency), undoOperationId: undoOperation.id },
      createdAt: now,
    });
    this.db.transaction(() => {
      this.db.deleteEvent(event.id);
      this.db.insertUndoOperation(undoOperation);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return { receipt, undoOperation };
  }

  undo(operationId?: string): { operation: UndoOperation; receipt: Receipt } {
    const now = toStorageIso(this.clock.now());
    const operation = operationId
      ? this.db.getUndoOperation(operationId)
      : this.db.getLatestUndoOperation(now);
    if (!operation) throw new Error("没有可撤销的近期操作");
    if (operation.undoneAt) throw new Error("这次操作已经撤销过了");
    if (operation.expiresAt < now) throw new Error("该操作已超过 10 分钟撤销窗口");
    const snapshots = operation.snapshot.eventSnapshots as unknown as EventSnapshot[];
    const removeEventIds = operation.snapshot.removeEventIds as unknown as string[];
    const receipt = this.receipt({
      type: "calendar_undone",
      eventId: snapshots[0]?.event.id ?? null,
      title: "已撤销操作",
      body: operation.summary,
      data: { operationId: operation.id },
      createdAt: now,
    });
    this.db.transaction(() => {
      for (const eventId of removeEventIds ?? []) this.db.deleteEvent(eventId);
      for (const snapshot of snapshots ?? []) {
        this.db.deleteEvent(snapshot.event.id);
        this.db.upsertEvent(snapshot.event);
        for (const override of snapshot.overrides) this.db.upsertOverride(override);
        for (const reminder of snapshot.reminders) this.db.upsertReminder(reminder);
        for (const weak of snapshot.weakReminders) this.db.upsertWeakReminder(weak);
      }
      this.db.markUndoOperationUsed(operation.id, now);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return { operation: { ...operation, undoneAt: now }, receipt };
  }

  confirmationToken(action: string, payload: Record<string, unknown>): string {
    return createHash("sha256").update(JSON.stringify({ action, payload })).digest("hex");
  }

  private requireConfirmation(
    action: string,
    payload: Record<string, unknown>,
    providedToken: string | undefined,
    message: string,
    conflicts: CalendarOccurrence[] = [],
  ): void {
    const token = this.confirmationToken(action, payload);
    if (providedToken !== token) {
      throw new CalendarConfirmationRequiredError(message, action, token, conflicts);
    }
  }

  private resolveEndAt(input: ModifyCalendarInput, occurrence: CalendarOccurrence, newStartAt: string): string | null {
    if (input.endsAt !== undefined) {
      if (input.endsAt === null) {
        if (occurrence.kind === "time_block") throw new Error("时间段日程不能清除结束时间");
        return null;
      }
      const end = toStorageIso(parseDateTime(input.endsAt, occurrence.timeZone));
      if (end <= newStartAt) throw new Error("日程结束时间必须晚于开始时间");
      return end;
    }
    if (!occurrence.effectiveEndAt) return null;
    const duration = parseDateTime(occurrence.effectiveEndAt, occurrence.timeZone).diff(
      parseDateTime(occurrence.effectiveStartAt, occurrence.timeZone),
    );
    return toStorageIso(parseDateTime(newStartAt, occurrence.timeZone).plus(duration));
  }

  private replaceOccurrenceReminders(
    event: CalendarEvent,
    originalStartAt: string,
    effectiveStartAt: string,
    now: string,
  ): void {
    const existing = this.db.getReminderByOccurrence(event.id, originalStartAt);
    const instances = this.calendarService.buildReminderInstances(event, originalStartAt, effectiveStartAt, now);
    if (existing) instances.reminder.id = existing.id;
    this.db.upsertReminder(instances.reminder);
    this.db.deleteWeakReminderByOccurrence(event.id, originalStartAt);
    if (instances.weakReminder) this.db.insertWeakReminder(instances.weakReminder);
  }

  private insertInitialReminders(event: CalendarEvent, now: string): void {
    const occurrence = parseDateTime(event.startAt, event.timeZone) >= this.clock.now().minus({ seconds: 1 })
      ? this.calendarService.getOccurrence(event.id, event.startAt)
      : event.recurrenceFrequency
        ? this.calendarService.nextOccurrenceOnOrAfter(event, toStorageIso(this.clock.now()))
        : null;
    if (!occurrence) return;
    const instances = this.calendarService.buildReminderInstances(
      event,
      occurrence.originalStartAt,
      occurrence.effectiveStartAt,
      now,
    );
    this.db.insertReminder(instances.reminder);
    if (instances.weakReminder) this.db.insertWeakReminder(instances.weakReminder);
  }

  private insertNextAvailableReminder(event: CalendarEvent, now: string): void {
    const next = this.calendarService.nextOccurrenceOnOrAfter(event, toStorageIso(this.clock.now()));
    if (!next) return;
    const instances = this.calendarService.buildReminderInstances(event, next.originalStartAt, next.effectiveStartAt, now);
    this.db.insertReminder(instances.reminder);
    if (instances.weakReminder) this.db.insertWeakReminder(instances.weakReminder);
  }

  private captureEvent(eventId: string): EventSnapshot {
    return {
      event: this.requireEvent(eventId),
      overrides: this.db.listOverrides(eventId),
      reminders: this.db.listRemindersForEvent(eventId),
      weakReminders: this.db.listWeakRemindersForEvent(eventId),
    };
  }

  private buildUndo(
    action: string,
    summary: string,
    eventSnapshots: EventSnapshot[],
    removeEventIds: string[],
    now: string,
  ): UndoOperation {
    return {
      id: randomUUID(),
      action,
      summary,
      snapshot: { eventSnapshots, removeEventIds },
      expiresAt: toStorageIso(parseDateTime(now, this.timeZone).plus({ minutes: 10 })),
      undoneAt: null,
      createdAt: now,
    };
  }

  private receipt(input: {
    type: Receipt["type"];
    eventId: string | null;
    title: string;
    body: string;
    data: Record<string, unknown>;
    createdAt: string;
  }): Receipt {
    return { id: randomUUID(), reminderId: null, ...input };
  }

  private requireEvent(eventId: string): CalendarEvent {
    const event = this.db.getEvent(eventId);
    if (!event) throw new Error("没有找到对应日程");
    return event;
  }

  private requireRecurringEvent(eventId: string): CalendarEvent {
    const event = this.requireEvent(eventId);
    if (!event.recurrenceFrequency) throw new Error("该操作只适用于周期日程");
    return event;
  }

  private requiredText(value: string, field: string): string {
    const text = value.trim();
    if (!text) throw new Error(`${field}不能为空`);
    return text;
  }

  private optionalText(value: string | null): string | null {
    return value?.trim() || null;
  }
}
