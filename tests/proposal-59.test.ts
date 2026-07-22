import { afterEach, describe, expect, it, vi } from "vitest";
import type { ProactiveVoiceItem } from "../src/adapters/proactive-voice.js";
import { CalendarConflictError } from "../src/services/calendar-service.js";
import { CalendarConfirmationRequiredError } from "../src/services/calendar-mutation-service.js";
import { createTestServices } from "./helpers.js";

const databases: Array<ReturnType<typeof createTestServices>["db"]> = [];

function setup(initialIso?: string) {
  const services = createTestServices(initialIso);
  databases.push(services.db);
  return services;
}

afterEach(() => {
  while (databases.length) databases.pop()!.close();
});

function confirmationToken(callback: () => unknown): string {
  try {
    callback();
  } catch (error) {
    if (error instanceof CalendarConfirmationRequiredError) return error.confirmationToken;
    throw error;
  }
  throw new Error("expected a confirmation challenge");
}

describe("PR #59 integrated proposal", () => {
  it("requires a duration for time blocks and detects interval overlap", () => {
    const { calendarService } = setup();
    expect(() => calendarService.create({
      title: "客户会议",
      startsAt: "2026-07-21T10:00:00+08:00",
      kind: "time_block",
    })).toThrow("结束时间或持续时长");

    const block = calendarService.create({
      title: "客户会议",
      startsAt: "2026-07-21T10:00:00+08:00",
      durationMinutes: 60,
    });
    expect(block.weakReminder?.triggerAt).toBe("2026-07-21T01:45:00.000Z");
    expect(() => calendarService.create({
      title: "打电话",
      startsAt: "2026-07-21T10:30:00+08:00",
    })).toThrow(CalendarConflictError);
  });

  it("allows an explicit opt-out from the default time-block weak reminder", () => {
    const { calendarService } = setup();
    const created = calendarService.create({
      title: "专注工作",
      startsAt: "2026-07-21T10:00:00+08:00",
      durationMinutes: 60,
      weakReminder: false,
    });
    expect(created.event.weakReminderEnabled).toBe(false);
    expect(created.weakReminder).toBeNull();
  });

  it("reconciles a missing upcoming default weak reminder after restart", () => {
    const { calendarService, db } = setup();
    const created = calendarService.create({
      title: "客户会议",
      startsAt: "2026-07-21T10:00:00+08:00",
      durationMinutes: 60,
    });
    db.deleteWeakReminderByOccurrence(created.event.id, created.event.startAt);
    expect(calendarService.reconcileUpcomingWeakReminders()).toBe(1);
    expect(db.getWeakReminderByOccurrence(created.event.id, created.event.startAt)?.triggerAt)
      .toBe("2026-07-21T01:45:00.000Z");
  });

  it("records non-sensitive short notes for 24 hours and rejects secrets", () => {
    const { shortNoteService, clock } = setup();
    shortNoteService.record({ content: "车停在 B2 的 C18", category: "parking" });
    expect(shortNoteService.query("车停").notes[0]?.content).toBe("车停在 B2 的 C18");
    expect(() => shortNoteService.record({ content: "取件码是 1234" })).toThrow("不能保存");
    clock.advance(24 * 60);
    expect(shortNoteService.query("车停").notes).toHaveLength(0);
  });

  it("delivers the optional weak reminder without creating an actionable due reminder", async () => {
    const deliver = vi.fn(async (
      _reminder: ProactiveVoiceItem["reminder"],
      _occurrence: ProactiveVoiceItem["occurrence"],
      _deliveryKind?: "main" | "weak",
    ) => ({ status: "delivered" as const }));
    const services = createTestServices("2026-07-21T09:00:00+08:00", {
      deliver,
      deliverBatch: vi.fn(async (_items: ProactiveVoiceItem[]) => ({ status: "delivered" as const })),
    });
    databases.push(services.db);
    services.calendarService.create({
      title: "路演",
      startsAt: "2026-07-21T09:20:00+08:00",
      weakReminderMinutes: 15,
    });
    services.clock.advance(5);
    await services.reminderService.scanDue();
    expect(services.reminderService.listDue()).toHaveLength(0);
    expect(services.db.listReceipts().some((receipt) => receipt.type === "reminder_weak_due")).toBe(true);
    expect(deliver.mock.calls[0]?.[2]).toBe("weak");
  });

  it("does not proactively speak after the third snooze comes due", async () => {
    const deliver = vi.fn(async () => ({ status: "delivered" as const }));
    const services = createTestServices("2026-07-21T09:00:00+08:00", { deliver });
    databases.push(services.db);
    const created = services.calendarService.create({
      title: "喝水",
      startsAt: "2026-07-21T09:01:00+08:00",
    });
    services.clock.advance(1);
    await services.reminderService.scanDue();
    for (let count = 1; count <= 3; count += 1) {
      services.reminderService.snooze(created.reminder.id, 1);
      services.clock.advance(1);
      await services.reminderService.scanDue();
    }
    expect(deliver).toHaveBeenCalledTimes(3);
    expect(services.reminderService.listDue()[0]?.reminder.snoozeCount).toBe(3);
  });

  it("modifies only one recurrence and can undo it within ten minutes", () => {
    const { calendarService, mutationService } = setup();
    const created = calendarService.create({
      title: "写日报",
      startsAt: "2026-07-21T17:30:00+08:00",
      recurrence: { frequency: "daily" },
    });
    const changed = mutationService.modify({
      eventId: created.event.id,
      originalStartAt: created.event.startAt,
      scope: "this_occurrence",
      newStartAt: "2026-07-21T19:00:00+08:00",
    });
    expect(changed.occurrence.effectiveStartAt).toBe("2026-07-21T11:00:00.000Z");
    const tomorrow = calendarService.query({
      rangeStart: "2026-07-22T00:00:00+08:00",
      rangeEnd: "2026-07-23T00:00:00+08:00",
    }).occurrences[0]!;
    expect(tomorrow.effectiveStartAt).toBe("2026-07-22T09:30:00.000Z");

    mutationService.undo(changed.undoOperation.id);
    const restored = calendarService.query({
      rangeStart: "2026-07-21T00:00:00+08:00",
      rangeEnd: "2026-07-22T00:00:00+08:00",
    }).occurrences[0]!;
    expect(restored.effectiveStartAt).toBe("2026-07-21T09:30:00.000Z");
  });

  it("splits this-and-future from the selected recurrence", () => {
    const { calendarService, mutationService } = setup();
    const created = calendarService.create({
      title: "写日报",
      startsAt: "2026-07-21T17:30:00+08:00",
      recurrence: { frequency: "daily" },
    });
    mutationService.modify({
      eventId: created.event.id,
      originalStartAt: "2026-07-22T09:30:00.000Z",
      scope: "this_and_future",
      newStartAt: "2026-07-22T19:00:00+08:00",
    });
    const queried = calendarService.query({
      rangeStart: "2026-07-21T00:00:00+08:00",
      rangeEnd: "2026-07-24T00:00:00+08:00",
    }).occurrences;
    expect(queried.map((item) => item.effectiveStartAt)).toEqual([
      "2026-07-21T09:30:00.000Z",
      "2026-07-22T11:00:00.000Z",
      "2026-07-23T11:00:00.000Z",
    ]);
  });

  it("confirms, skips idempotently, and restores a recurrence", () => {
    const { calendarService, mutationService } = setup();
    const created = calendarService.create({
      title: "晨会",
      startsAt: "2026-07-21T10:00:00+08:00",
      recurrence: { frequency: "daily" },
    });
    const input = { eventId: created.event.id, originalStartAt: created.event.startAt };
    const token = confirmationToken(() => mutationService.skip(input));
    const skipped = mutationService.skip({ ...input, confirmationToken: token });
    expect(skipped.alreadySkipped).toBe(false);
    expect(mutationService.skip(input).alreadySkipped).toBe(true);
    expect(calendarService.query({
      rangeStart: "2026-07-21T00:00:00+08:00",
      rangeEnd: "2026-07-22T00:00:00+08:00",
    }).occurrences).toHaveLength(0);
    mutationService.undo(skipped.undoOperation!.id);
    expect(calendarService.query({
      rangeStart: "2026-07-21T00:00:00+08:00",
      rangeEnd: "2026-07-22T00:00:00+08:00",
    }).occurrences).toHaveLength(1);
  });

  it("pauses, resumes, and confirms termination of a recurring series", () => {
    const { calendarService, mutationService } = setup();
    const created = calendarService.create({
      title: "晨会",
      startsAt: "2026-07-21T10:00:00+08:00",
      recurrence: { frequency: "daily" },
    });
    mutationService.pause({
      eventId: created.event.id,
      until: "2026-07-23T00:00:00+08:00",
    });
    expect(calendarService.query({
      rangeStart: "2026-07-21T00:00:00+08:00",
      rangeEnd: "2026-07-23T00:00:00+08:00",
    }).occurrences).toHaveLength(0);
    mutationService.resume(created.event.id);
    expect(calendarService.query({
      rangeStart: "2026-07-21T00:00:00+08:00",
      rangeEnd: "2026-07-23T00:00:00+08:00",
    }).occurrences).toHaveLength(2);

    const from = "2026-07-22T02:00:00.000Z";
    const token = confirmationToken(() => mutationService.terminate({
      eventId: created.event.id,
      from,
    }));
    mutationService.terminate({ eventId: created.event.id, from, confirmationToken: token });
    expect(calendarService.query({
      rangeStart: "2026-07-21T00:00:00+08:00",
      rangeEnd: "2026-07-24T00:00:00+08:00",
    }).occurrences).toHaveLength(1);
  });

  it("confirms deletion and restores the event with undo", () => {
    const { calendarService, mutationService } = setup();
    const created = calendarService.create({
      title: "客户回访",
      startsAt: "2026-07-21T15:00:00+08:00",
    });
    const token = confirmationToken(() => mutationService.delete({ eventId: created.event.id }));
    const deleted = mutationService.delete({ eventId: created.event.id, confirmationToken: token });
    expect(calendarService.getEvent(created.event.id)).toBeNull();
    mutationService.undo(deleted.undoOperation.id);
    expect(calendarService.getEvent(created.event.id)?.title).toBe("客户回访");
  });

  it("requires confirmation when a reschedule enters an occupied interval", () => {
    const { calendarService, mutationService } = setup();
    calendarService.create({
      title: "客户会议",
      startsAt: "2026-07-21T10:00:00+08:00",
      durationMinutes: 60,
    });
    const created = calendarService.create({
      title: "打电话",
      startsAt: "2026-07-21T12:00:00+08:00",
    });
    expect(() => mutationService.modify({
      eventId: created.event.id,
      originalStartAt: created.event.startAt,
      scope: "this_occurrence",
      newStartAt: "2026-07-21T10:30:00+08:00",
    })).toThrow(CalendarConfirmationRequiredError);
    expect(calendarService.getEvent(created.event.id)?.startAt).toBe(created.event.startAt);
  });
});
