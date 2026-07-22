import { mkdirSync } from "node:fs";
import path from "node:path";
import { DatabaseSync } from "node:sqlite";
import type {
  CalendarEvent,
  OccurrenceOverride,
  Receipt,
  ReminderInstance,
  ReminderStatus,
  ShortNote,
  UndoOperation,
  WeakReminderInstance,
} from "../domain/types.js";

type Row = Record<string, unknown>;

function nullableString(value: unknown): string | null {
  return typeof value === "string" ? value : null;
}

function eventFromRow(row: Row): CalendarEvent {
  const kind = (nullableString(row.kind) ?? "point") as CalendarEvent["kind"];
  return {
    id: String(row.id),
    title: String(row.title),
    startAt: String(row.start_at),
    endAt: nullableString(row.end_at),
    kind,
    timeZone: String(row.time_zone),
    location: nullableString(row.location),
    notes: nullableString(row.notes),
    recurrenceFrequency: nullableString(row.recurrence_frequency) as CalendarEvent["recurrenceFrequency"],
    recurrenceWeekday: row.recurrence_weekday == null ? null : Number(row.recurrence_weekday),
    recurrenceMonthDay: row.recurrence_month_day == null ? null : Number(row.recurrence_month_day),
    reminderOffsetMinutes: Number(row.reminder_offset_minutes),
    weakReminderMinutes: row.weak_reminder_minutes == null ? null : Number(row.weak_reminder_minutes),
    weakReminderEnabled: row.weak_reminder_enabled == null
      ? kind === "time_block"
      : Number(row.weak_reminder_enabled) === 1,
    pausedFrom: nullableString(row.paused_from),
    pausedUntil: nullableString(row.paused_until),
    terminatedAt: nullableString(row.terminated_at),
    createdAt: String(row.created_at),
    updatedAt: nullableString(row.updated_at) ?? String(row.created_at),
  };
}

function overrideFromRow(row: Row): OccurrenceOverride {
  return {
    id: String(row.id),
    eventId: String(row.event_id),
    originalStartAt: String(row.original_start_at),
    newStartAt: nullableString(row.new_start_at),
    patch: row.patch_json ? JSON.parse(String(row.patch_json)) as OccurrenceOverride["patch"] : {},
    status: String(row.status) as OccurrenceOverride["status"],
    createdAt: String(row.created_at),
    updatedAt: String(row.updated_at),
  };
}

function weakReminderFromRow(row: Row): WeakReminderInstance {
  return {
    id: String(row.id),
    eventId: String(row.event_id),
    originalStartAt: String(row.original_start_at),
    effectiveStartAt: String(row.effective_start_at),
    triggerAt: String(row.trigger_at),
    status: String(row.status) as WeakReminderInstance["status"],
    deliveredAt: nullableString(row.delivered_at),
    createdAt: String(row.created_at),
    updatedAt: String(row.updated_at),
  };
}

function shortNoteFromRow(row: Row): ShortNote {
  return {
    id: String(row.id),
    content: String(row.content),
    category: nullableString(row.category),
    expiresAt: String(row.expires_at),
    createdAt: String(row.created_at),
  };
}

function undoOperationFromRow(row: Row): UndoOperation {
  return {
    id: String(row.id),
    action: String(row.action),
    summary: String(row.summary),
    snapshot: JSON.parse(String(row.snapshot_json)) as Record<string, unknown>,
    expiresAt: String(row.expires_at),
    undoneAt: nullableString(row.undone_at),
    createdAt: String(row.created_at),
  };
}

function reminderFromRow(row: Row): ReminderInstance {
  return {
    id: String(row.id),
    eventId: String(row.event_id),
    originalStartAt: String(row.original_start_at),
    effectiveStartAt: String(row.effective_start_at),
    triggerAt: String(row.trigger_at),
    status: String(row.status) as ReminderStatus,
    snoozeCount: Number(row.snooze_count),
    voiceDeliveredAt: nullableString(row.voice_delivered_at),
    closedAt: nullableString(row.closed_at),
    createdAt: String(row.created_at),
    updatedAt: String(row.updated_at),
  };
}

function receiptFromRow(row: Row): Receipt {
  return {
    id: String(row.id),
    type: String(row.type) as Receipt["type"],
    eventId: nullableString(row.event_id),
    reminderId: nullableString(row.reminder_id),
    title: String(row.title),
    body: String(row.body),
    data: JSON.parse(String(row.data_json)) as Record<string, unknown>,
    createdAt: String(row.created_at),
  };
}

export class CalendarDatabase {
  private readonly db: DatabaseSync;

  public constructor(databasePath: string) {
    if (databasePath !== ":memory:") mkdirSync(path.dirname(databasePath), { recursive: true });
    this.db = new DatabaseSync(databasePath);
    this.db.exec("PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL;");
    this.migrate();
  }

  private migrate(): void {
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS calendar_events (
        id TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        start_at TEXT NOT NULL,
        end_at TEXT,
        kind TEXT NOT NULL DEFAULT 'point' CHECK (kind IN ('point', 'time_block')),
        time_zone TEXT NOT NULL,
        location TEXT,
        notes TEXT,
        recurrence_frequency TEXT CHECK (recurrence_frequency IN ('daily', 'weekly', 'monthly')),
        recurrence_weekday INTEGER,
        recurrence_month_day INTEGER,
        reminder_offset_minutes INTEGER NOT NULL DEFAULT 0,
        weak_reminder_minutes INTEGER,
        weak_reminder_enabled INTEGER CHECK (weak_reminder_enabled IN (0, 1)),
        paused_from TEXT,
        paused_until TEXT,
        terminated_at TEXT,
        created_at TEXT NOT NULL,
        updated_at TEXT NOT NULL
      );

      CREATE TABLE IF NOT EXISTS occurrence_overrides (
        id TEXT PRIMARY KEY,
        event_id TEXT NOT NULL REFERENCES calendar_events(id) ON DELETE CASCADE,
        original_start_at TEXT NOT NULL,
        new_start_at TEXT,
        patch_json TEXT NOT NULL DEFAULT '{}',
        status TEXT NOT NULL CHECK (status IN ('moved', 'skipped')),
        created_at TEXT NOT NULL,
        updated_at TEXT NOT NULL,
        UNIQUE(event_id, original_start_at)
      );

      CREATE TABLE IF NOT EXISTS reminder_instances (
        id TEXT PRIMARY KEY,
        event_id TEXT NOT NULL REFERENCES calendar_events(id) ON DELETE CASCADE,
        original_start_at TEXT NOT NULL,
        effective_start_at TEXT NOT NULL,
        trigger_at TEXT NOT NULL,
        status TEXT NOT NULL CHECK (status IN ('scheduled', 'pushed', 'snoozed', 'closed')),
        snooze_count INTEGER NOT NULL DEFAULT 0,
        voice_delivered_at TEXT,
        closed_at TEXT,
        created_at TEXT NOT NULL,
        updated_at TEXT NOT NULL,
        UNIQUE(event_id, original_start_at)
      );

      CREATE INDEX IF NOT EXISTS idx_reminders_due
        ON reminder_instances(status, trigger_at);
      CREATE INDEX IF NOT EXISTS idx_overrides_new_start
        ON occurrence_overrides(new_start_at);

      CREATE TABLE IF NOT EXISTS receipts (
        id TEXT PRIMARY KEY,
        type TEXT NOT NULL,
        event_id TEXT REFERENCES calendar_events(id) ON DELETE SET NULL,
        reminder_id TEXT REFERENCES reminder_instances(id) ON DELETE SET NULL,
        title TEXT NOT NULL,
        body TEXT NOT NULL,
        data_json TEXT NOT NULL,
        created_at TEXT NOT NULL
      );

      CREATE INDEX IF NOT EXISTS idx_receipts_created
        ON receipts(created_at);

      CREATE TABLE IF NOT EXISTS weak_reminder_instances (
        id TEXT PRIMARY KEY,
        event_id TEXT NOT NULL REFERENCES calendar_events(id) ON DELETE CASCADE,
        original_start_at TEXT NOT NULL,
        effective_start_at TEXT NOT NULL,
        trigger_at TEXT NOT NULL,
        status TEXT NOT NULL CHECK (status IN ('scheduled', 'delivered', 'cancelled')),
        delivered_at TEXT,
        created_at TEXT NOT NULL,
        updated_at TEXT NOT NULL,
        UNIQUE(event_id, original_start_at)
      );

      CREATE INDEX IF NOT EXISTS idx_weak_reminders_due
        ON weak_reminder_instances(status, trigger_at);

      CREATE TABLE IF NOT EXISTS short_notes (
        id TEXT PRIMARY KEY,
        content TEXT NOT NULL,
        category TEXT,
        expires_at TEXT NOT NULL,
        created_at TEXT NOT NULL
      );

      CREATE INDEX IF NOT EXISTS idx_short_notes_expiry
        ON short_notes(expires_at, created_at);

      CREATE TABLE IF NOT EXISTS undo_operations (
        id TEXT PRIMARY KEY,
        action TEXT NOT NULL,
        summary TEXT NOT NULL,
        snapshot_json TEXT NOT NULL,
        expires_at TEXT NOT NULL,
        undone_at TEXT,
        created_at TEXT NOT NULL
      );
    `);

    this.ensureColumn("calendar_events", "end_at", "TEXT");
    this.ensureColumn("calendar_events", "kind", "TEXT NOT NULL DEFAULT 'point'");
    this.ensureColumn("calendar_events", "weak_reminder_minutes", "INTEGER");
    this.ensureColumn("calendar_events", "weak_reminder_enabled", "INTEGER");
    this.ensureColumn("calendar_events", "paused_from", "TEXT");
    this.ensureColumn("calendar_events", "paused_until", "TEXT");
    this.ensureColumn("calendar_events", "terminated_at", "TEXT");
    this.ensureColumn("calendar_events", "updated_at", "TEXT");
    this.ensureColumn("occurrence_overrides", "patch_json", "TEXT NOT NULL DEFAULT '{}'");
    this.db.exec("UPDATE calendar_events SET updated_at = created_at WHERE updated_at IS NULL");
  }

  private ensureColumn(table: string, column: string, definition: string): void {
    const rows = this.db.prepare(`PRAGMA table_info(${table})`).all() as Row[];
    if (rows.some((row) => String(row.name) === column)) return;
    this.db.exec(`ALTER TABLE ${table} ADD COLUMN ${column} ${definition}`);
  }

  close(): void {
    this.db.close();
  }

  transaction<T>(callback: () => T): T {
    this.db.exec("BEGIN IMMEDIATE");
    try {
      const result = callback();
      this.db.exec("COMMIT");
      return result;
    } catch (error) {
      this.db.exec("ROLLBACK");
      throw error;
    }
  }

  insertEvent(event: CalendarEvent): void {
    this.db.prepare(`
      INSERT INTO calendar_events (
        id, title, start_at, end_at, kind, time_zone, location, notes,
        recurrence_frequency, recurrence_weekday, recurrence_month_day,
        reminder_offset_minutes, weak_reminder_minutes, weak_reminder_enabled, paused_from, paused_until,
        terminated_at, created_at, updated_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    `).run(
      event.id,
      event.title,
      event.startAt,
      event.endAt,
      event.kind,
      event.timeZone,
      event.location,
      event.notes,
      event.recurrenceFrequency,
      event.recurrenceWeekday,
      event.recurrenceMonthDay,
      event.reminderOffsetMinutes,
      event.weakReminderMinutes,
      event.weakReminderEnabled ? 1 : 0,
      event.pausedFrom,
      event.pausedUntil,
      event.terminatedAt,
      event.createdAt,
      event.updatedAt,
    );
  }

  upsertEvent(event: CalendarEvent): void {
    this.db.prepare(`
      INSERT INTO calendar_events (
        id, title, start_at, end_at, kind, time_zone, location, notes,
        recurrence_frequency, recurrence_weekday, recurrence_month_day,
        reminder_offset_minutes, weak_reminder_minutes, weak_reminder_enabled, paused_from, paused_until,
        terminated_at, created_at, updated_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(id) DO UPDATE SET
        title = excluded.title, start_at = excluded.start_at, end_at = excluded.end_at,
        kind = excluded.kind, time_zone = excluded.time_zone, location = excluded.location,
        notes = excluded.notes, recurrence_frequency = excluded.recurrence_frequency,
        recurrence_weekday = excluded.recurrence_weekday,
        recurrence_month_day = excluded.recurrence_month_day,
        reminder_offset_minutes = excluded.reminder_offset_minutes,
        weak_reminder_minutes = excluded.weak_reminder_minutes,
        weak_reminder_enabled = excluded.weak_reminder_enabled,
        paused_from = excluded.paused_from, paused_until = excluded.paused_until,
        terminated_at = excluded.terminated_at, updated_at = excluded.updated_at
    `).run(
      event.id, event.title, event.startAt, event.endAt, event.kind, event.timeZone,
      event.location, event.notes, event.recurrenceFrequency, event.recurrenceWeekday,
      event.recurrenceMonthDay, event.reminderOffsetMinutes, event.weakReminderMinutes,
      event.weakReminderEnabled ? 1 : 0, event.pausedFrom, event.pausedUntil,
      event.terminatedAt, event.createdAt, event.updatedAt,
    );
  }

  deleteEvent(id: string): void {
    this.db.prepare("DELETE FROM calendar_events WHERE id = ?").run(id);
  }

  getEvent(id: string): CalendarEvent | null {
    const row = this.db.prepare("SELECT * FROM calendar_events WHERE id = ?").get(id) as Row | undefined;
    return row ? eventFromRow(row) : null;
  }

  listEvents(): CalendarEvent[] {
    return (this.db.prepare("SELECT * FROM calendar_events ORDER BY start_at").all() as Row[]).map(eventFromRow);
  }

  findEventsByTitle(query: string): CalendarEvent[] {
    const escaped = query.replaceAll("%", "\\%").replaceAll("_", "\\_");
    return (this.db.prepare(`
      SELECT * FROM calendar_events
      WHERE title LIKE ? ESCAPE '\\' COLLATE NOCASE
      ORDER BY start_at
    `).all(`%${escaped}%`) as Row[]).map(eventFromRow);
  }

  getOverride(eventId: string, originalStartAt: string): OccurrenceOverride | null {
    const row = this.db.prepare(`
      SELECT * FROM occurrence_overrides WHERE event_id = ? AND original_start_at = ?
    `).get(eventId, originalStartAt) as Row | undefined;
    return row ? overrideFromRow(row) : null;
  }

  listOverrides(eventId: string): OccurrenceOverride[] {
    return (this.db.prepare(`
      SELECT * FROM occurrence_overrides WHERE event_id = ? ORDER BY original_start_at
    `).all(eventId) as Row[]).map(overrideFromRow);
  }

  listMovedOverridesBetween(rangeStart: string, rangeEnd: string): OccurrenceOverride[] {
    return (this.db.prepare(`
      SELECT * FROM occurrence_overrides
      WHERE status = 'moved' AND new_start_at >= ? AND new_start_at < ?
      ORDER BY new_start_at
    `).all(rangeStart, rangeEnd) as Row[]).map(overrideFromRow);
  }

  upsertOverride(override: OccurrenceOverride): void {
    this.db.prepare(`
      INSERT INTO occurrence_overrides (
        id, event_id, original_start_at, new_start_at, patch_json, status, created_at, updated_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(event_id, original_start_at) DO UPDATE SET
        new_start_at = excluded.new_start_at,
        patch_json = excluded.patch_json,
        status = excluded.status,
        updated_at = excluded.updated_at
    `).run(
      override.id,
      override.eventId,
      override.originalStartAt,
      override.newStartAt,
      JSON.stringify(override.patch),
      override.status,
      override.createdAt,
      override.updatedAt,
    );
  }

  deleteOverride(eventId: string, originalStartAt: string): void {
    this.db.prepare(`
      DELETE FROM occurrence_overrides WHERE event_id = ? AND original_start_at = ?
    `).run(eventId, originalStartAt);
  }

  deleteOverrides(eventId: string): void {
    this.db.prepare("DELETE FROM occurrence_overrides WHERE event_id = ?").run(eventId);
  }

  insertReminder(reminder: ReminderInstance): void {
    this.db.prepare(`
      INSERT OR IGNORE INTO reminder_instances (
        id, event_id, original_start_at, effective_start_at, trigger_at,
        status, snooze_count, voice_delivered_at, closed_at, created_at, updated_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    `).run(
      reminder.id,
      reminder.eventId,
      reminder.originalStartAt,
      reminder.effectiveStartAt,
      reminder.triggerAt,
      reminder.status,
      reminder.snoozeCount,
      reminder.voiceDeliveredAt,
      reminder.closedAt,
      reminder.createdAt,
      reminder.updatedAt,
    );
  }

  upsertReminder(reminder: ReminderInstance): void {
    this.db.prepare(`
      INSERT INTO reminder_instances (
        id, event_id, original_start_at, effective_start_at, trigger_at,
        status, snooze_count, voice_delivered_at, closed_at, created_at, updated_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(event_id, original_start_at) DO UPDATE SET
        id = excluded.id, effective_start_at = excluded.effective_start_at,
        trigger_at = excluded.trigger_at, status = excluded.status,
        snooze_count = excluded.snooze_count, voice_delivered_at = excluded.voice_delivered_at,
        closed_at = excluded.closed_at, updated_at = excluded.updated_at
    `).run(
      reminder.id, reminder.eventId, reminder.originalStartAt, reminder.effectiveStartAt,
      reminder.triggerAt, reminder.status, reminder.snoozeCount, reminder.voiceDeliveredAt,
      reminder.closedAt, reminder.createdAt, reminder.updatedAt,
    );
  }

  getReminder(id: string): ReminderInstance | null {
    const row = this.db.prepare("SELECT * FROM reminder_instances WHERE id = ?").get(id) as Row | undefined;
    return row ? reminderFromRow(row) : null;
  }

  getReminderByOccurrence(eventId: string, originalStartAt: string): ReminderInstance | null {
    const row = this.db.prepare(`
      SELECT * FROM reminder_instances WHERE event_id = ? AND original_start_at = ?
    `).get(eventId, originalStartAt) as Row | undefined;
    return row ? reminderFromRow(row) : null;
  }

  listReadyToPush(now: string): ReminderInstance[] {
    return (this.db.prepare(`
      SELECT * FROM reminder_instances
      WHERE status IN ('scheduled', 'snoozed') AND trigger_at <= ?
      ORDER BY trigger_at, created_at
    `).all(now) as Row[]).map(reminderFromRow);
  }

  listPushed(): ReminderInstance[] {
    return (this.db.prepare(`
      SELECT * FROM reminder_instances
      WHERE status = 'pushed'
      ORDER BY trigger_at, created_at
    `).all() as Row[]).map(reminderFromRow);
  }

  markReminderPushed(id: string, now: string): void {
    this.db.prepare(`
      UPDATE reminder_instances
      SET status = 'pushed', updated_at = ?
      WHERE id = ? AND status IN ('scheduled', 'snoozed')
    `).run(now, id);
  }

  markReminderVoiceDelivered(id: string, now: string): void {
    this.db.prepare(`
      UPDATE reminder_instances SET voice_delivered_at = COALESCE(voice_delivered_at, ?), updated_at = ?
      WHERE id = ?
    `).run(now, now, id);
  }

  closeReminder(id: string, now: string): void {
    this.db.prepare(`
      UPDATE reminder_instances
      SET status = 'closed', closed_at = ?, updated_at = ?
      WHERE id = ? AND status != 'closed'
    `).run(now, now, id);
  }

  snoozeReminder(id: string, triggerAt: string, snoozeCount: number, now: string): void {
    this.db.prepare(`
      UPDATE reminder_instances
      SET status = 'snoozed', trigger_at = ?, snooze_count = ?,
          voice_delivered_at = NULL, updated_at = ?
      WHERE id = ? AND status = 'pushed'
    `).run(triggerAt, snoozeCount, now, id);
  }

  resetReminderForMovedOccurrence(
    id: string,
    effectiveStartAt: string,
    triggerAt: string,
    now: string,
  ): void {
    this.db.prepare(`
      UPDATE reminder_instances
      SET effective_start_at = ?, trigger_at = ?, status = 'scheduled',
          snooze_count = 0, voice_delivered_at = NULL, closed_at = NULL, updated_at = ?
      WHERE id = ?
    `).run(effectiveStartAt, triggerAt, now, id);
  }

  deleteReminderByOccurrence(eventId: string, originalStartAt: string): void {
    this.db.prepare(`
      DELETE FROM reminder_instances WHERE event_id = ? AND original_start_at = ?
    `).run(eventId, originalStartAt);
  }

  listRemindersForEvent(eventId: string): ReminderInstance[] {
    return (this.db.prepare(`
      SELECT * FROM reminder_instances WHERE event_id = ? ORDER BY original_start_at
    `).all(eventId) as Row[]).map(reminderFromRow);
  }

  deleteRemindersFrom(eventId: string, originalStartAt: string): void {
    this.db.prepare(`
      DELETE FROM reminder_instances WHERE event_id = ? AND original_start_at >= ?
    `).run(eventId, originalStartAt);
  }

  insertWeakReminder(reminder: WeakReminderInstance): void {
    this.db.prepare(`
      INSERT OR IGNORE INTO weak_reminder_instances (
        id, event_id, original_start_at, effective_start_at, trigger_at,
        status, delivered_at, created_at, updated_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    `).run(
      reminder.id, reminder.eventId, reminder.originalStartAt, reminder.effectiveStartAt,
      reminder.triggerAt, reminder.status, reminder.deliveredAt, reminder.createdAt,
      reminder.updatedAt,
    );
  }

  upsertWeakReminder(reminder: WeakReminderInstance): void {
    this.db.prepare(`
      INSERT INTO weak_reminder_instances (
        id, event_id, original_start_at, effective_start_at, trigger_at,
        status, delivered_at, created_at, updated_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(event_id, original_start_at) DO UPDATE SET
        id = excluded.id, effective_start_at = excluded.effective_start_at,
        trigger_at = excluded.trigger_at, status = excluded.status,
        delivered_at = excluded.delivered_at, updated_at = excluded.updated_at
    `).run(
      reminder.id, reminder.eventId, reminder.originalStartAt, reminder.effectiveStartAt,
      reminder.triggerAt, reminder.status, reminder.deliveredAt, reminder.createdAt,
      reminder.updatedAt,
    );
  }

  getWeakReminderByOccurrence(eventId: string, originalStartAt: string): WeakReminderInstance | null {
    const row = this.db.prepare(`
      SELECT * FROM weak_reminder_instances WHERE event_id = ? AND original_start_at = ?
    `).get(eventId, originalStartAt) as Row | undefined;
    return row ? weakReminderFromRow(row) : null;
  }

  listWeakRemindersForEvent(eventId: string): WeakReminderInstance[] {
    return (this.db.prepare(`
      SELECT * FROM weak_reminder_instances WHERE event_id = ? ORDER BY original_start_at
    `).all(eventId) as Row[]).map(weakReminderFromRow);
  }

  deleteWeakRemindersFrom(eventId: string, originalStartAt: string): void {
    this.db.prepare(`
      DELETE FROM weak_reminder_instances WHERE event_id = ? AND original_start_at >= ?
    `).run(eventId, originalStartAt);
  }

  listReadyWeakReminders(now: string): WeakReminderInstance[] {
    return (this.db.prepare(`
      SELECT * FROM weak_reminder_instances
      WHERE status = 'scheduled' AND trigger_at <= ?
      ORDER BY trigger_at, created_at
    `).all(now) as Row[]).map(weakReminderFromRow);
  }

  markWeakReminderDelivered(id: string, now: string): void {
    this.db.prepare(`
      UPDATE weak_reminder_instances
      SET status = 'delivered', delivered_at = ?, updated_at = ?
      WHERE id = ? AND status = 'scheduled'
    `).run(now, now, id);
  }

  deleteWeakReminderByOccurrence(eventId: string, originalStartAt: string): void {
    this.db.prepare(`
      DELETE FROM weak_reminder_instances WHERE event_id = ? AND original_start_at = ?
    `).run(eventId, originalStartAt);
  }

  insertShortNote(note: ShortNote): void {
    this.db.prepare(`
      INSERT INTO short_notes (id, content, category, expires_at, created_at)
      VALUES (?, ?, ?, ?, ?)
    `).run(note.id, note.content, note.category, note.expiresAt, note.createdAt);
  }

  listActiveShortNotes(now: string, query?: string): ShortNote[] {
    if (!query) {
      return (this.db.prepare(`
        SELECT * FROM short_notes WHERE expires_at > ? ORDER BY created_at DESC
      `).all(now) as Row[]).map(shortNoteFromRow);
    }
    const escaped = query.replaceAll("%", "\\%").replaceAll("_", "\\_");
    return (this.db.prepare(`
      SELECT * FROM short_notes
      WHERE expires_at > ? AND content LIKE ? ESCAPE '\\' COLLATE NOCASE
      ORDER BY created_at DESC
    `).all(now, `%${escaped}%`) as Row[]).map(shortNoteFromRow);
  }

  insertUndoOperation(operation: UndoOperation): void {
    this.db.prepare(`
      INSERT INTO undo_operations (
        id, action, summary, snapshot_json, expires_at, undone_at, created_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?)
    `).run(
      operation.id, operation.action, operation.summary, JSON.stringify(operation.snapshot),
      operation.expiresAt, operation.undoneAt, operation.createdAt,
    );
  }

  getUndoOperation(id: string): UndoOperation | null {
    const row = this.db.prepare("SELECT * FROM undo_operations WHERE id = ?").get(id) as Row | undefined;
    return row ? undoOperationFromRow(row) : null;
  }

  getLatestUndoOperation(now: string): UndoOperation | null {
    const row = this.db.prepare(`
      SELECT * FROM undo_operations
      WHERE undone_at IS NULL AND expires_at >= ?
      ORDER BY created_at DESC, rowid DESC LIMIT 1
    `).get(now) as Row | undefined;
    return row ? undoOperationFromRow(row) : null;
  }

  markUndoOperationUsed(id: string, now: string): void {
    this.db.prepare(`
      UPDATE undo_operations SET undone_at = ? WHERE id = ? AND undone_at IS NULL
    `).run(now, id);
  }

  insertReceipt(receipt: Receipt): void {
    this.db.prepare(`
      INSERT INTO receipts (
        id, type, event_id, reminder_id, title, body, data_json, created_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    `).run(
      receipt.id,
      receipt.type,
      receipt.eventId,
      receipt.reminderId,
      receipt.title,
      receipt.body,
      JSON.stringify(receipt.data),
      receipt.createdAt,
    );
  }

  listReceipts(limit = 100): Receipt[] {
    const rows = this.db.prepare(`
      SELECT * FROM receipts ORDER BY created_at DESC, rowid DESC LIMIT ?
    `).all(limit) as Row[];
    return rows.map(receiptFromRow).reverse();
  }
}
