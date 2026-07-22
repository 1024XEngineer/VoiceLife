import { afterEach, describe, expect, it } from "vitest";
import { CalendarConflictError } from "../src/services/calendar-service.js";
import { createTestServices } from "./helpers.js";

const openDatabases: Array<ReturnType<typeof createTestServices>["db"]> = [];

function setup(initialIso?: string) {
  const services = createTestServices(initialIso);
  openDatabases.push(services.db);
  return services;
}

afterEach(() => {
  while (openDatabases.length) openDatabases.pop()!.close();
});

describe("CalendarService", () => {
  it("creates and queries a one-off event with a receipt", () => {
    const { calendarService, db } = setup();
    const created = calendarService.create({
      title: "客户拜访",
      startsAt: "2026-07-22T10:00:00+08:00",
      location: "A 座 1503",
    });

    expect(created.event.title).toBe("客户拜访");
    expect(created.reminder.triggerAt).toBe(created.event.startAt);
    expect(created.receipt.data.location).toBe("A 座 1503");

    const queried = calendarService.query({
      rangeStart: "2026-07-22T00:00:00+08:00",
      rangeEnd: "2026-07-23T00:00:00+08:00",
    });
    expect(queried.occurrences).toHaveLength(1);
    expect(queried.occurrences[0]).toMatchObject({ title: "客户拜访", location: "A 座 1503" });
    expect(db.listReceipts()).toHaveLength(2);
  });

  it("rejects past times, invalid reminder ordering, and inconsistent recurrence", () => {
    const { calendarService } = setup();
    expect(() =>
      calendarService.create({ title: "过去的会", startsAt: "2026-07-21T08:00:00+08:00" }),
    ).toThrow("已经过去");
    expect(() =>
      calendarService.create({
        title: "客户拜访",
        startsAt: "2026-07-22T10:00:00+08:00",
        remindAt: "2026-07-22T11:00:00+08:00",
      }),
    ).toThrow("不能晚于");
    expect(() =>
      calendarService.create({
        title: "周报",
        startsAt: "2026-07-24T17:00:00+08:00",
        recurrence: { frequency: "weekly", weekday: 1 },
      }),
    ).toThrow("星期设置不一致");
  });

  it("requires explicit confirmation before creating an exact-time conflict", () => {
    const { calendarService, db } = setup();
    calendarService.create({ title: "开会", startsAt: "2026-07-21T11:00:00+08:00" });

    let conflict: CalendarConflictError | undefined;
    try {
      calendarService.create({ title: "吃饭", startsAt: "2026-07-21T11:00:00+08:00" });
    } catch (error) {
      if (error instanceof CalendarConflictError) conflict = error;
      else throw error;
    }

    expect(conflict).toMatchObject({
      requestedTitle: "吃饭",
      requestedStartAt: "2026-07-21T03:00:00.000Z",
    });
    expect(conflict?.conflicts).toHaveLength(1);
    expect(conflict?.conflicts[0]?.title).toBe("开会");
    expect(conflict?.confirmationToken).toMatch(/^[a-f0-9]{64}$/);
    expect(db.listEvents()).toHaveLength(1);
    expect(db.listReceipts()).toHaveLength(1);

    const confirmed = calendarService.create({
      title: "吃饭",
      startsAt: "2026-07-21T11:00:00+08:00",
      conflictConfirmationToken: conflict!.confirmationToken,
    });
    expect(confirmed.conflicts.map((item) => item.title)).toEqual(["开会"]);
    expect(db.listEvents()).toHaveLength(2);
    expect(db.listReceipts()).toHaveLength(2);
  });

  it("supports daily, weekly, and monthly occurrences", () => {
    const { calendarService } = setup("2026-01-01T09:00:00+08:00");
    calendarService.create({
      title: "日报",
      startsAt: "2026-01-01T17:30:00+08:00",
      recurrence: { frequency: "daily" },
    });
    calendarService.create({
      title: "周报",
      startsAt: "2026-01-02T17:00:00+08:00",
      recurrence: { frequency: "weekly", weekday: 5 },
    });
    calendarService.create({
      title: "月末盘点",
      startsAt: "2026-01-31T20:00:00+08:00",
      recurrence: { frequency: "monthly", monthDay: 31 },
    });

    const queried = calendarService.query({
      rangeStart: "2026-03-01T00:00:00+08:00",
      rangeEnd: "2026-04-01T00:00:00+08:00",
    });
    expect(queried.occurrences.some((item) => item.title === "日报")).toBe(true);
    expect(queried.occurrences.some((item) => item.title === "周报")).toBe(true);
    expect(
      queried.occurrences.some(
        (item) => item.title === "月末盘点" && item.effectiveStartAt === "2026-03-31T12:00:00.000Z",
      ),
    ).toBe(true);
  });

  it("moves only today's daily occurrence and keeps tomorrow unchanged", () => {
    const { calendarService } = setup();
    const created = calendarService.create({
      title: "写日报",
      startsAt: "2026-07-21T17:30:00+08:00",
      recurrence: { frequency: "daily" },
    });

    const found = calendarService.find({
      query: "日报",
      rangeStart: "2026-07-21T00:00:00+08:00",
      rangeEnd: "2026-07-22T00:00:00+08:00",
    });
    expect(found).toHaveLength(1);

    const updated = calendarService.rescheduleOccurrence({
      eventId: created.event.id,
      originalStartAt: found[0]!.originalStartAt,
      newStartAt: "2026-07-21T19:00:00+08:00",
    });
    expect(updated.occurrence.moved).toBe(true);
    expect(updated.receipt.data.scope).toBe("this_occurrence");

    const today = calendarService.query({
      rangeStart: "2026-07-21T00:00:00+08:00",
      rangeEnd: "2026-07-22T00:00:00+08:00",
    });
    const tomorrow = calendarService.query({
      rangeStart: "2026-07-22T00:00:00+08:00",
      rangeEnd: "2026-07-23T00:00:00+08:00",
    });
    expect(today.occurrences[0]!.effectiveStartAt).toBe("2026-07-21T11:00:00.000Z");
    expect(tomorrow.occurrences[0]!.effectiveStartAt).toBe("2026-07-22T09:30:00.000Z");
  });

  it("returns all candidates instead of guessing", () => {
    const { calendarService } = setup();
    calendarService.create({ title: "项目评审 A", startsAt: "2026-07-21T14:00:00+08:00" });
    calendarService.create({ title: "项目评审 B", startsAt: "2026-07-21T15:00:00+08:00" });
    const found = calendarService.find({ query: "项目评审" });
    expect(found).toHaveLength(2);
    expect(found.map((item) => item.title)).toEqual(["项目评审 A", "项目评审 B"]);
  });
});
