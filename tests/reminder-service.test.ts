import { afterEach, describe, expect, it, vi } from "vitest";
import type { ProactiveVoiceItem } from "../src/adapters/proactive-voice.js";
import { CalendarConflictError } from "../src/services/calendar-service.js";
import { createTestServices } from "./helpers.js";

const databases: Array<ReturnType<typeof createTestServices>["db"]> = [];
function setup() {
  const services = createTestServices();
  databases.push(services.db);
  return services;
}
afterEach(() => {
  while (databases.length) databases.pop()!.close();
});

describe("ReminderService", () => {
  it("moves through push, snooze, re-push and close without changing the schedule", async () => {
    const { calendarService, reminderService, clock } = setup();
    const created = calendarService.create({
      title: "吃药",
      startsAt: "2026-07-21T09:01:00+08:00",
    });

    clock.advance(1);
    await reminderService.scanDue();
    const due = reminderService.listDue();
    expect(due).toHaveLength(1);
    expect(due[0]!.reminder.status).toBe("pushed");

    const snoozed = reminderService.snooze(created.reminder.id, 10);
    expect(snoozed.reminder.status).toBe("snoozed");
    expect(snoozed.reminder.snoozeCount).toBe(1);
    expect(reminderService.snooze(created.reminder.id, 10).alreadySnoozed).toBe(true);

    clock.advance(10);
    await reminderService.scanDue();
    expect(reminderService.listDue()[0]!.reminder.status).toBe("pushed");

    const closed = reminderService.close(created.reminder.id);
    expect(closed.reminder.status).toBe("closed");
    expect(closed.receipt!.data.scheduleChanged).toBe(false);
    expect(reminderService.close(created.reminder.id).alreadyClosed).toBe(true);

    const event = calendarService.getEvent(created.event.id);
    expect(event!.startAt).toBe(created.event.startAt);
  });

  it("records voice delivery only after proactive playback succeeds", async () => {
    const deliver = vi.fn(async () => ({ status: "delivered" as const }));
    const services = createTestServices("2026-07-21T09:00:00+08:00", { deliver });
    databases.push(services.db);
    const created = services.calendarService.create({
      title: "写日报",
      startsAt: "2026-07-21T09:01:00+08:00",
    });

    services.clock.advance(1);
    await services.reminderService.scanDue();

    expect(deliver).toHaveBeenCalledOnce();
    expect(services.db.getReminder(created.reminder.id)?.voiceDeliveredAt).not.toBeNull();
  });

  it("allows three snoozes and rejects a fourth", async () => {
    const { calendarService, reminderService, clock } = setup();
    const created = calendarService.create({ title: "喝水", startsAt: "2026-07-21T09:01:00+08:00" });
    clock.advance(1);
    await reminderService.scanDue();

    for (let count = 1; count <= 3; count += 1) {
      const snoozed = reminderService.snooze(created.reminder.id, 1);
      expect(snoozed.reminder.snoozeCount).toBe(count);
      clock.advance(1);
      await reminderService.scanDue();
    }
    expect(() => reminderService.snooze(created.reminder.id, 1)).toThrow("已经推迟三次");
  });

  it("keeps simultaneous due reminders separate", async () => {
    const deliver = vi.fn(async () => ({ status: "delivered" as const }));
    const deliverBatch = vi.fn(async (_items: ProactiveVoiceItem[]) => ({
      status: "delivered" as const,
    }));
    const services = createTestServices(
      "2026-07-21T09:00:00+08:00",
      { deliver, deliverBatch },
    );
    databases.push(services.db);
    const { calendarService, reminderService, clock } = services;
    calendarService.create({ title: "吃药", startsAt: "2026-07-21T09:01:00+08:00" });
    let confirmationToken = "";
    try {
      calendarService.create({ title: "出门开会", startsAt: "2026-07-21T09:01:00+08:00" });
    } catch (error) {
      if (error instanceof CalendarConflictError) confirmationToken = error.confirmationToken;
      else throw error;
    }
    calendarService.create({
      title: "出门开会",
      startsAt: "2026-07-21T09:01:00+08:00",
      conflictConfirmationToken: confirmationToken,
    });
    clock.advance(1);
    await reminderService.scanDue();
    const due = reminderService.listDue();
    expect(due).toHaveLength(2);
    expect(new Set(due.map((item) => item.reminder.id)).size).toBe(2);
    expect(deliver).not.toHaveBeenCalled();
    expect(deliverBatch).toHaveBeenCalledOnce();
    expect(deliverBatch.mock.calls[0]![0].map((item) => item.occurrence.title)).toEqual([
      "吃药",
      "出门开会",
    ]);
    expect(due.every((item) => item.reminder.voiceDeliveredAt)).toBe(true);
  });

  it("creates the next recurring reminder when the current one is due", async () => {
    const { calendarService, reminderService, clock } = setup();
    const created = calendarService.create({
      title: "日报",
      startsAt: "2026-07-21T09:01:00+08:00",
      recurrence: { frequency: "daily" },
    });
    clock.advance(1);
    await reminderService.scanDue();
    const tomorrow = calendarService.query({
      rangeStart: "2026-07-22T00:00:00+08:00",
      rangeEnd: "2026-07-23T00:00:00+08:00",
    }).occurrences[0]!;
    expect(tomorrow.title).toBe("日报");
    expect(tomorrow.originalStartAt).not.toBe(created.event.startAt);
  });
});
