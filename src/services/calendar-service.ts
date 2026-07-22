import { createHash, randomUUID } from "node:crypto";
import { DateTime } from "luxon";
import type {
  CalendarEvent,
  CalendarOccurrence,
  OccurrenceOverride,
  Receipt,
  RecurrenceInput,
  ReminderInstance,
  WeakReminderInstance,
} from "../domain/types.js";
import {
  formatChineseDateTime,
  nextOccurrence,
  originalOccurrencesBetween,
  parseDateTime,
  toStorageIso,
} from "../domain/recurrence.js";
import { CalendarDatabase } from "../storage/database.js";
import type { Clock } from "./clock.js";
import { ReceiptBus } from "./receipt-bus.js";

export interface CreateCalendarInput {
  title: string;
  startsAt: string;
  endsAt?: string;
  durationMinutes?: number;
  kind?: "point" | "time_block";
  remindAt?: string;
  weakReminder?: boolean;
  weakReminderMinutes?: 15;
  location?: string;
  notes?: string;
  recurrence?: RecurrenceInput;
  conflictConfirmationToken?: string;
}

export interface CalendarQueryInput {
  rangeStart: string;
  rangeEnd: string;
}

export interface CalendarFindInput {
  query: string;
  rangeStart?: string;
  rangeEnd?: string;
}

function nonEmpty(value: string, field: string): string {
  const trimmed = value.trim();
  if (!trimmed) throw new Error(`${field}不能为空`);
  return trimmed;
}

export class CalendarConflictError extends Error {
  public constructor(
    public readonly requestedTitle: string,
    public readonly requestedStartAt: string,
    public readonly requestedEndAt: string | null,
    public readonly conflicts: CalendarOccurrence[],
    public readonly confirmationToken: string,
  ) {
    super("该时间已有日程，需要用户确认后才能继续创建");
    this.name = "CalendarConflictError";
  }
}

export class CalendarService {
  public constructor(
    private readonly db: CalendarDatabase,
    private readonly clock: Clock,
    private readonly receiptBus: ReceiptBus,
    private readonly timeZone: string,
  ) {}

  create(input: CreateCalendarInput): {
    event: CalendarEvent;
    reminder: ReminderInstance;
    weakReminder: WeakReminderInstance | null;
    receipt: Receipt;
    nextOccurrenceAt: string | null;
    conflicts: CalendarOccurrence[];
  } {
    const title = nonEmpty(input.title, "标题");
    const now = this.clock.now();
    const startsAt = parseDateTime(input.startsAt, this.timeZone);
    if (startsAt < now.minus({ seconds: 1 })) {
      throw new Error("日程时间已经过去，请明确是今天还是明天");
    }

    const remindAt = input.remindAt
      ? parseDateTime(input.remindAt, this.timeZone)
      : startsAt;
    if (remindAt > startsAt) throw new Error("提醒时间不能晚于日程发生时间");
    if (remindAt < now.minus({ seconds: 1 })) throw new Error("提醒时间已经过去");

    const kind = input.kind ?? (input.endsAt || input.durationMinutes ? "time_block" : "point");
    if (input.endsAt && input.durationMinutes != null) {
      throw new Error("结束时间和持续时长只能填写一个");
    }
    if (input.durationMinutes != null && (!Number.isInteger(input.durationMinutes) || input.durationMinutes < 1)) {
      throw new Error("持续时长必须是正整数分钟");
    }
    const endsAt = input.endsAt
      ? parseDateTime(input.endsAt, this.timeZone)
      : input.durationMinutes != null
        ? startsAt.plus({ minutes: input.durationMinutes })
        : null;
    if (kind === "time_block" && !endsAt) throw new Error("会议、拜访、课程等时间段日程必须提供结束时间或持续时长");
    if (kind === "point" && endsAt) throw new Error("时间点提醒不能设置结束时间，请改为时间段日程");
    if (endsAt && endsAt <= startsAt) throw new Error("日程结束时间必须晚于开始时间");
    if (input.weakReminderMinutes != null && input.weakReminderMinutes !== 15) {
      throw new Error("MVP 的提前弱提醒固定为 15 分钟");
    }

    this.validateRecurrence(input.recurrence, startsAt);
    const storedStartAt = toStorageIso(startsAt);
    const storedEndAt = endsAt ? toStorageIso(endsAt) : null;
    const conflicts = this.findConflicts(storedStartAt, storedEndAt, kind);
    const confirmationToken = this.conflictConfirmationToken(title, storedStartAt, storedEndAt, kind, conflicts);
    if (
      conflicts.length > 0 &&
      input.conflictConfirmationToken !== confirmationToken
    ) {
      throw new CalendarConflictError(title, storedStartAt, storedEndAt, conflicts, confirmationToken);
    }
    const createdAt = toStorageIso(now);
    const reminderOffsetMinutes = Math.round(startsAt.diff(remindAt, "minutes").minutes);
    const weakReminderEnabled = input.weakReminder
      ?? (input.weakReminderMinutes != null || kind === "time_block");
    const event: CalendarEvent = {
      id: randomUUID(),
      title,
      startAt: storedStartAt,
      endAt: storedEndAt,
      kind,
      timeZone: this.timeZone,
      location: input.location?.trim() || null,
      notes: input.notes?.trim() || null,
      recurrenceFrequency: input.recurrence?.frequency ?? null,
      recurrenceWeekday:
        input.recurrence?.frequency === "weekly"
          ? (input.recurrence.weekday ?? startsAt.weekday)
          : null,
      recurrenceMonthDay:
        input.recurrence?.frequency === "monthly"
          ? (input.recurrence.monthDay ?? startsAt.day)
          : null,
      reminderOffsetMinutes,
      weakReminderMinutes: weakReminderEnabled ? 15 : null,
      weakReminderEnabled,
      pausedFrom: null,
      pausedUntil: null,
      terminatedAt: null,
      createdAt,
      updatedAt: createdAt,
    };
    const reminder = this.buildReminder(event, storedStartAt, storedStartAt, createdAt);
    const weakReminder = this.buildWeakReminder(event, storedStartAt, storedStartAt, createdAt);
    const next = nextOccurrence(event, storedStartAt);
    const nextOccurrenceAt = next ? toStorageIso(next) : null;
    const recurrenceText = this.recurrenceText(event);
    const receipt = this.buildReceipt({
      type: "calendar_created",
      eventId: event.id,
      reminderId: reminder.id,
      title: "已创建日程",
      body: `${event.title} · ${formatChineseDateTime(event.startAt, event.timeZone)}${recurrenceText ? ` · ${recurrenceText}` : ""}`,
      data: {
        title: event.title,
        startsAt: event.startAt,
        endsAt: event.endAt,
        kind: event.kind,
        remindAt: reminder.triggerAt,
        weakRemindAt: weakReminder?.triggerAt ?? null,
        weakReminderEnabled: event.weakReminderEnabled,
        location: event.location,
        notes: event.notes,
        recurrence: recurrenceText || null,
        nextOccurrenceAt,
      },
      createdAt,
    });

    this.db.transaction(() => {
      this.db.insertEvent(event);
      this.db.insertReminder(reminder);
      if (weakReminder) this.db.insertWeakReminder(weakReminder);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return { event, reminder, weakReminder, receipt, nextOccurrenceAt, conflicts };
  }

  query(input: CalendarQueryInput): { occurrences: CalendarOccurrence[]; receipt: Receipt } {
    const rangeStart = parseDateTime(input.rangeStart, this.timeZone);
    const rangeEnd = parseDateTime(input.rangeEnd, this.timeZone);
    if (rangeEnd <= rangeStart) throw new Error("查询结束时间必须晚于开始时间");
    const occurrences = this.collectOccurrences(rangeStart, rangeEnd);
    const now = toStorageIso(this.clock.now());
    const receipt = this.buildReceipt({
      type: "calendar_query",
      eventId: null,
      reminderId: null,
      title: "日程查询结果",
      body: occurrences.length
        ? occurrences
            .map((item) => `${formatChineseDateTime(item.effectiveStartAt, item.timeZone)} ${item.title}`)
            .join("\n")
        : "这个时间范围内没有日程",
      data: { rangeStart: toStorageIso(rangeStart), rangeEnd: toStorageIso(rangeEnd), occurrences },
      createdAt: now,
    });
    this.db.insertReceipt(receipt);
    this.receiptBus.publish(receipt);
    return { occurrences, receipt };
  }

  find(input: CalendarFindInput): CalendarOccurrence[] {
    const events = this.db.findEventsByTitle(nonEmpty(input.query, "查询关键词"));
    const rangeStart = input.rangeStart
      ? parseDateTime(input.rangeStart, this.timeZone)
      : this.clock.now().startOf("day");
    const rangeEnd = input.rangeEnd
      ? parseDateTime(input.rangeEnd, this.timeZone)
      : rangeStart.plus({ years: 1 });
    const ids = new Set(events.map((event) => event.id));
    return this.collectOccurrences(rangeStart, rangeEnd).filter((item) => ids.has(item.eventId));
  }

  rescheduleOccurrence(input: {
    eventId: string;
    originalStartAt: string;
    newStartAt: string;
  }): {
    occurrence: CalendarOccurrence;
    receipt: Receipt;
    nextOccurrenceAt: string | null;
  } {
    const event = this.requireEvent(input.eventId);
    const originalStartAt = toStorageIso(parseDateTime(input.originalStartAt, event.timeZone));
    this.assertOccurrenceExists(event, originalStartAt);
    const newStart = parseDateTime(input.newStartAt, event.timeZone);
    if (newStart < this.clock.now().minus({ seconds: 1 })) throw new Error("新的日程时间已经过去");
    const newStartAt = toStorageIso(newStart);
    const now = toStorageIso(this.clock.now());
    const existingOverride = this.db.getOverride(event.id, originalStartAt);
    const override: OccurrenceOverride = {
      id: existingOverride?.id ?? randomUUID(),
      eventId: event.id,
      originalStartAt,
      newStartAt,
      patch: existingOverride?.patch ?? {},
      status: "moved",
      createdAt: existingOverride?.createdAt ?? now,
      updatedAt: now,
    };

    let reminder = this.db.getReminderByOccurrence(event.id, originalStartAt);
    const triggerAt = toStorageIso(newStart.minus({ minutes: event.reminderOffsetMinutes }));
    if (!reminder) {
      reminder = this.buildReminder(event, originalStartAt, newStartAt, now);
    }
    const weakReminder = this.buildWeakReminder(event, originalStartAt, newStartAt, now);

    const next = nextOccurrence(event, originalStartAt);
    const nextOccurrenceAt = next ? toStorageIso(next) : null;
    const receipt = this.buildReceipt({
      type: "calendar_rescheduled",
      eventId: event.id,
      reminderId: reminder.id,
      title: "已修改本次日程",
      body: `${event.title}：${formatChineseDateTime(originalStartAt, event.timeZone)} → ${formatChineseDateTime(newStartAt, event.timeZone)}；仅本次`,
      data: {
        title: event.title,
        originalStartAt,
        newStartAt,
        scope: "this_occurrence",
        nextOccurrenceAt,
      },
      createdAt: now,
    });

    this.db.transaction(() => {
      this.db.upsertOverride(override);
      const existingReminder = this.db.getReminder(reminder!.id);
      if (existingReminder) {
        this.db.resetReminderForMovedOccurrence(reminder!.id, newStartAt, triggerAt, now);
      } else {
        this.db.insertReminder(reminder!);
      }
      this.db.deleteWeakReminderByOccurrence(event.id, originalStartAt);
      if (weakReminder) this.db.insertWeakReminder(weakReminder);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return {
      occurrence: this.toOccurrence(event, originalStartAt, override),
      receipt,
      nextOccurrenceAt,
    };
  }

  getOccurrenceForReminder(reminder: ReminderInstance): CalendarOccurrence {
    const event = this.requireEvent(reminder.eventId);
    const override = this.db.getOverride(event.id, reminder.originalStartAt);
    return this.toOccurrence(event, reminder.originalStartAt, override);
  }

  getOccurrence(eventId: string, originalStartAt: string): CalendarOccurrence {
    const event = this.requireEvent(eventId);
    this.assertOccurrenceExists(event, originalStartAt);
    const override = this.db.getOverride(event.id, originalStartAt);
    return this.toOccurrence(event, originalStartAt, override);
  }

  findConflictsForChange(
    startsAt: string,
    endsAt: string | null,
    kind: CalendarEvent["kind"],
    excludedEventId: string,
  ): CalendarOccurrence[] {
    return this.findConflicts(startsAt, endsAt, kind, excludedEventId);
  }

  buildReminderInstances(
    event: CalendarEvent,
    originalStartAt: string,
    effectiveStartAt: string,
    createdAt: string,
  ): { reminder: ReminderInstance; weakReminder: WeakReminderInstance | null } {
    return {
      reminder: this.buildReminder(event, originalStartAt, effectiveStartAt, createdAt),
      weakReminder: this.buildWeakReminder(event, originalStartAt, effectiveStartAt, createdAt),
    };
  }

  nextOccurrenceOnOrAfter(event: CalendarEvent, from: string): CalendarOccurrence | null {
    const rangeStart = parseDateTime(from, event.timeZone);
    const rangeEnd = rangeStart.plus({ years: 5 });
    for (const originalStartAt of originalOccurrencesBetween(event, rangeStart, rangeEnd, 5000)) {
      if (this.isPausedOccurrence(event, originalStartAt)) continue;
      const override = this.db.getOverride(event.id, originalStartAt);
      if (override?.status === "skipped") continue;
      return this.toOccurrence(event, originalStartAt, override);
    }
    return null;
  }

  reconcileUpcomingWeakReminders(): number {
    const now = toStorageIso(this.clock.now());
    let inserted = 0;
    for (const event of this.db.listEvents()) {
      if (!event.weakReminderEnabled) continue;
      const occurrence = this.nextOccurrenceOnOrAfter(event, now);
      if (!occurrence) continue;
      if (this.db.getWeakReminderByOccurrence(event.id, occurrence.originalStartAt)) continue;
      const weakReminder = this.buildWeakReminder(
        event,
        occurrence.originalStartAt,
        occurrence.effectiveStartAt,
        now,
      );
      if (!weakReminder) continue;
      this.db.insertWeakReminder(weakReminder);
      inserted += 1;
    }
    return inserted;
  }

  ensureNextReminder(eventId: string, afterOriginalStartAt: string): ReminderInstance | null {
    const event = this.requireEvent(eventId);
    let cursor = afterOriginalStartAt;
    for (let attempt = 0; attempt < 500; attempt += 1) {
      const next = nextOccurrence(event, cursor);
      if (!next) return null;
      const originalStartAt = toStorageIso(next);
      cursor = originalStartAt;
      const existing = this.db.getReminderByOccurrence(event.id, originalStartAt);
      if (existing) return existing;
      const override = this.db.getOverride(event.id, originalStartAt);
      if (override?.status === "skipped" || this.isPausedOccurrence(event, originalStartAt)) continue;
      const effectiveStartAt = override?.newStartAt ?? originalStartAt;
      const createdAt = toStorageIso(this.clock.now());
      const reminder = this.buildReminder(event, originalStartAt, effectiveStartAt, createdAt);
      this.db.insertReminder(reminder);
      const weakReminder = this.buildWeakReminder(event, originalStartAt, effectiveStartAt, createdAt);
      if (weakReminder) this.db.insertWeakReminder(weakReminder);
      return reminder;
    }
    throw new Error("无法在支持范围内找到下一次有效日程");
  }

  getEvent(id: string): CalendarEvent | null {
    return this.db.getEvent(id);
  }

  private validateRecurrence(recurrence: RecurrenceInput | undefined, startsAt: DateTime): void {
    if (!recurrence) return;
    if (recurrence.frequency === "weekly" && recurrence.weekday != null) {
      if (!Number.isInteger(recurrence.weekday) || recurrence.weekday < 1 || recurrence.weekday > 7) {
        throw new Error("每周周期的 weekday 必须是 1 到 7");
      }
      if (recurrence.weekday !== startsAt.weekday) throw new Error("首次发生日期与每周星期设置不一致");
    }
    if (recurrence.frequency === "monthly" && recurrence.monthDay != null) {
      if (!Number.isInteger(recurrence.monthDay) || recurrence.monthDay < 1 || recurrence.monthDay > 31) {
        throw new Error("每月周期的 monthDay 必须是 1 到 31");
      }
      if (recurrence.monthDay !== startsAt.day) throw new Error("首次发生日期与每月日期设置不一致");
    }
  }

  private conflictConfirmationToken(
    title: string,
    startsAt: string,
    endsAt: string | null,
    kind: CalendarEvent["kind"],
    conflicts: CalendarOccurrence[],
  ): string {
    const eventIds = conflicts.map((item) => item.eventId).sort();
    return createHash("sha256")
      .update(JSON.stringify({ title, startsAt, endsAt, kind, eventIds }))
      .digest("hex");
  }

  private collectOccurrences(rangeStart: DateTime, rangeEnd: DateTime): CalendarOccurrence[] {
    const items = new Map<string, CalendarOccurrence>();
    for (const event of this.db.listEvents()) {
      const overrides = new Map(
        this.db.listOverrides(event.id).map((override) => [override.originalStartAt, override]),
      );
      for (const originalStartAt of originalOccurrencesBetween(event, rangeStart, rangeEnd)) {
        if (this.isPausedOccurrence(event, originalStartAt)) continue;
        const override = overrides.get(originalStartAt) ?? null;
        if (override?.status === "skipped") continue;
        const occurrence = this.toOccurrence(event, originalStartAt, override);
        const effective = parseDateTime(occurrence.effectiveStartAt, event.timeZone);
        if (effective >= rangeStart && effective < rangeEnd) {
          items.set(`${event.id}:${originalStartAt}`, occurrence);
        }
      }
    }

    for (const override of this.db.listMovedOverridesBetween(toStorageIso(rangeStart), toStorageIso(rangeEnd))) {
      const event = this.db.getEvent(override.eventId);
      if (!event) continue;
      if (event.terminatedAt && override.originalStartAt >= event.terminatedAt) continue;
      if (this.isPausedOccurrence(event, override.originalStartAt)) continue;
      items.set(
        `${event.id}:${override.originalStartAt}`,
        this.toOccurrence(event, override.originalStartAt, override),
      );
    }

    return [...items.values()].sort((left, right) =>
      left.effectiveStartAt.localeCompare(right.effectiveStartAt),
    );
  }

  private assertOccurrenceExists(event: CalendarEvent, originalStartAt: string): void {
    const target = parseDateTime(originalStartAt, event.timeZone);
    const matches = originalOccurrencesBetween(
      event,
      target.minus({ seconds: 1 }),
      target.plus({ seconds: 1 }),
      5000,
    );
    if (!matches.includes(originalStartAt)) throw new Error("指定时间不是该日程的有效周期实例");
  }

  private buildReminder(
    event: CalendarEvent,
    originalStartAt: string,
    effectiveStartAt: string,
    createdAt: string,
  ): ReminderInstance {
    const effective = parseDateTime(effectiveStartAt, event.timeZone);
    return {
      id: randomUUID(),
      eventId: event.id,
      originalStartAt,
      effectiveStartAt,
      triggerAt: toStorageIso(effective.minus({ minutes: event.reminderOffsetMinutes })),
      status: "scheduled",
      snoozeCount: 0,
      voiceDeliveredAt: null,
      closedAt: null,
      createdAt,
      updatedAt: createdAt,
    };
  }

  private buildWeakReminder(
    event: CalendarEvent,
    originalStartAt: string,
    effectiveStartAt: string,
    createdAt: string,
  ): WeakReminderInstance | null {
    if (!event.weakReminderEnabled) return null;
    const weakReminderMinutes = event.weakReminderMinutes ?? 15;
    const triggerAt = parseDateTime(effectiveStartAt, event.timeZone).minus({
      minutes: weakReminderMinutes,
    });
    if (triggerAt < this.clock.now().minus({ seconds: 1 })) return null;
    return {
      id: randomUUID(),
      eventId: event.id,
      originalStartAt,
      effectiveStartAt,
      triggerAt: toStorageIso(triggerAt),
      status: "scheduled",
      deliveredAt: null,
      createdAt,
      updatedAt: createdAt,
    };
  }

  private toOccurrence(
    event: CalendarEvent,
    originalStartAt: string,
    override: OccurrenceOverride | null,
  ): CalendarOccurrence {
    const effectiveStartAt = override?.newStartAt ?? originalStartAt;
    const baseDuration = event.endAt
      ? parseDateTime(event.endAt, event.timeZone).diff(
          parseDateTime(event.startAt, event.timeZone),
        )
      : null;
    const derivedEndAt = baseDuration
      ? toStorageIso(parseDateTime(effectiveStartAt, event.timeZone).plus(baseDuration))
      : null;
    return {
      eventId: event.id,
      title: override?.patch.title ?? event.title,
      originalStartAt,
      effectiveStartAt,
      effectiveEndAt: override?.patch.endAt === undefined ? derivedEndAt : override.patch.endAt,
      kind: event.kind,
      timeZone: event.timeZone,
      location: override?.patch.location === undefined ? event.location : override.patch.location,
      notes: override?.patch.notes === undefined ? event.notes : override.patch.notes,
      recurrenceFrequency: event.recurrenceFrequency,
      moved: override?.status === "moved",
    };
  }

  private findConflicts(
    startsAt: string,
    endsAt: string | null,
    kind: CalendarEvent["kind"],
    excludedEventId?: string,
  ): CalendarOccurrence[] {
    const requestedStart = parseDateTime(startsAt, this.timeZone);
    const requestedEnd = endsAt ? parseDateTime(endsAt, this.timeZone) : null;
    const candidates = this.collectOccurrences(
      requestedStart.minus({ years: 1 }),
      (requestedEnd ?? requestedStart).plus({ seconds: 1 }),
    );
    return candidates.filter((candidate) => {
      if (candidate.eventId === excludedEventId) return false;
      const candidateStart = parseDateTime(candidate.effectiveStartAt, candidate.timeZone);
      const candidateEnd = candidate.effectiveEndAt
        ? parseDateTime(candidate.effectiveEndAt, candidate.timeZone)
        : null;
      if (kind === "point" && candidate.kind === "point") {
        return requestedStart.toMillis() === candidateStart.toMillis();
      }
      if (kind === "point") {
        return candidateEnd != null && requestedStart >= candidateStart && requestedStart < candidateEnd;
      }
      if (candidate.kind === "point") {
        return requestedEnd != null && candidateStart >= requestedStart && candidateStart < requestedEnd;
      }
      return requestedEnd != null && candidateEnd != null && requestedStart < candidateEnd && candidateStart < requestedEnd;
    });
  }

  private isPausedOccurrence(event: CalendarEvent, originalStartAt: string): boolean {
    if (!event.pausedFrom || !event.pausedUntil) return false;
    return originalStartAt >= event.pausedFrom && originalStartAt < event.pausedUntil;
  }

  private requireEvent(id: string): CalendarEvent {
    const event = this.db.getEvent(id);
    if (!event) throw new Error("没有找到对应日程");
    return event;
  }

  private recurrenceText(event: CalendarEvent): string {
    if (event.recurrenceFrequency === "daily") return "每天";
    if (event.recurrenceFrequency === "weekly") return `每周${event.recurrenceWeekday}`;
    if (event.recurrenceFrequency === "monthly") return `每月${event.recurrenceMonthDay}号`;
    return "";
  }

  private buildReceipt(input: Omit<Receipt, "id">): Receipt {
    return { id: randomUUID(), ...input };
  }
}
