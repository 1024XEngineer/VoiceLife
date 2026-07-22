import { DateTime } from "luxon";
import { UnsupportedProactiveVoiceAdapter } from "../src/adapters/proactive-voice.js";
import type { ProactiveVoiceAdapter } from "../src/adapters/proactive-voice.js";
import { CalendarService } from "../src/services/calendar-service.js";
import { CalendarMutationService } from "../src/services/calendar-mutation-service.js";
import { DemoClock } from "../src/services/clock.js";
import { ReceiptBus } from "../src/services/receipt-bus.js";
import { ReminderService } from "../src/services/reminder-service.js";
import { ShortNoteService } from "../src/services/short-note-service.js";
import { CalendarDatabase } from "../src/storage/database.js";

export function createTestServices(
  initialIso = "2026-07-21T09:00:00+08:00",
  proactiveVoice: ProactiveVoiceAdapter = new UnsupportedProactiveVoiceAdapter(),
) {
  const timeZone = "Asia/Shanghai";
  const db = new CalendarDatabase(":memory:");
  const clock = new DemoClock(
    timeZone,
    DateTime.fromISO(initialIso, { setZone: true }).setZone(timeZone),
  );
  const receiptBus = new ReceiptBus();
  const calendarService = new CalendarService(db, clock, receiptBus, timeZone);
  const mutationService = new CalendarMutationService(
    db,
    calendarService,
    clock,
    receiptBus,
    timeZone,
  );
  const shortNoteService = new ShortNoteService(db, clock, receiptBus);
  const reminderService = new ReminderService(
    db,
    calendarService,
    clock,
    receiptBus,
    proactiveVoice,
  );
  return {
    timeZone,
    db,
    clock,
    receiptBus,
    calendarService,
    reminderService,
    mutationService,
    shortNoteService,
  };
}
