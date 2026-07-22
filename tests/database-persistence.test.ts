import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { DatabaseSync } from "node:sqlite";
import path from "node:path";
import { DateTime } from "luxon";
import { afterEach, describe, expect, it } from "vitest";
import { CalendarService } from "../src/services/calendar-service.js";
import { DemoClock } from "../src/services/clock.js";
import { ReceiptBus } from "../src/services/receipt-bus.js";
import { CalendarDatabase } from "../src/storage/database.js";

const temporaryDirectories: string[] = [];

afterEach(() => {
  while (temporaryDirectories.length) {
    rmSync(temporaryDirectories.pop()!, { recursive: true, force: true });
  }
});

describe("CalendarDatabase persistence", () => {
  it("restores events and receipts after the service restarts", () => {
    const directory = mkdtempSync(path.join(tmpdir(), "linx-calendar-test-"));
    temporaryDirectories.push(directory);
    const databasePath = path.join(directory, "calendar.sqlite");
    const timeZone = "Asia/Shanghai";
    const firstDatabase = new CalendarDatabase(databasePath);
    const firstService = new CalendarService(
      firstDatabase,
      new DemoClock(timeZone, DateTime.fromISO("2026-07-21T09:00:00+08:00")),
      new ReceiptBus(),
      timeZone,
    );
    const created = firstService.create({
      title: "客户拜访",
      startsAt: "2026-07-22T10:00:00+08:00",
    });
    firstDatabase.close();

    const reopenedDatabase = new CalendarDatabase(databasePath);
    expect(reopenedDatabase.getEvent(created.event.id)).toMatchObject({ title: "客户拜访" });
    expect(reopenedDatabase.getReminder(created.reminder.id)).toMatchObject({ status: "scheduled" });
    expect(reopenedDatabase.listReceipts()).toHaveLength(1);
    reopenedDatabase.close();
  });

  it("migrates an existing narrow prototype database without losing events", () => {
    const directory = mkdtempSync(path.join(tmpdir(), "linx-calendar-migration-"));
    temporaryDirectories.push(directory);
    const databasePath = path.join(directory, "calendar.sqlite");
    const legacy = new DatabaseSync(databasePath);
    legacy.exec(`
      CREATE TABLE calendar_events (
        id TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        start_at TEXT NOT NULL,
        time_zone TEXT NOT NULL,
        location TEXT,
        notes TEXT,
        recurrence_frequency TEXT,
        recurrence_weekday INTEGER,
        recurrence_month_day INTEGER,
        reminder_offset_minutes INTEGER NOT NULL DEFAULT 0,
        created_at TEXT NOT NULL
      );
      CREATE TABLE occurrence_overrides (
        id TEXT PRIMARY KEY,
        event_id TEXT NOT NULL REFERENCES calendar_events(id) ON DELETE CASCADE,
        original_start_at TEXT NOT NULL,
        new_start_at TEXT,
        status TEXT NOT NULL,
        created_at TEXT NOT NULL,
        updated_at TEXT NOT NULL,
        UNIQUE(event_id, original_start_at)
      );
      CREATE TABLE reminder_instances (
        id TEXT PRIMARY KEY,
        event_id TEXT NOT NULL REFERENCES calendar_events(id) ON DELETE CASCADE,
        original_start_at TEXT NOT NULL,
        effective_start_at TEXT NOT NULL,
        trigger_at TEXT NOT NULL,
        status TEXT NOT NULL,
        snooze_count INTEGER NOT NULL DEFAULT 0,
        voice_delivered_at TEXT,
        closed_at TEXT,
        created_at TEXT NOT NULL,
        updated_at TEXT NOT NULL,
        UNIQUE(event_id, original_start_at)
      );
      CREATE TABLE receipts (
        id TEXT PRIMARY KEY,
        type TEXT NOT NULL,
        event_id TEXT REFERENCES calendar_events(id) ON DELETE SET NULL,
        reminder_id TEXT REFERENCES reminder_instances(id) ON DELETE SET NULL,
        title TEXT NOT NULL,
        body TEXT NOT NULL,
        data_json TEXT NOT NULL,
        created_at TEXT NOT NULL
      );
      INSERT INTO calendar_events (
        id, title, start_at, time_zone, reminder_offset_minutes, created_at
      ) VALUES (
        'legacy-event', '旧版日报', '2026-07-21T09:30:00.000Z',
        'Asia/Shanghai', 0, '2026-07-21T01:00:00.000Z'
      );
    `);
    legacy.close();

    const migrated = new CalendarDatabase(databasePath);
    expect(migrated.getEvent("legacy-event")).toMatchObject({
      title: "旧版日报",
      kind: "point",
      endAt: null,
      weakReminderEnabled: false,
      updatedAt: "2026-07-21T01:00:00.000Z",
    });
    migrated.close();
  });
});
