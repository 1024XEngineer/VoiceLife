export type RecurrenceFrequency = "daily" | "weekly" | "monthly";
export type OverrideStatus = "moved" | "skipped";
export type ReminderStatus = "scheduled" | "pushed" | "snoozed" | "closed";
export type EventKind = "point" | "time_block";
export type RecurrenceScope = "this_occurrence" | "this_and_future" | "entire_series";
export type WeakReminderStatus = "scheduled" | "delivered" | "cancelled";

export interface CalendarEvent {
  id: string;
  title: string;
  startAt: string;
  endAt: string | null;
  kind: EventKind;
  timeZone: string;
  location: string | null;
  notes: string | null;
  recurrenceFrequency: RecurrenceFrequency | null;
  recurrenceWeekday: number | null;
  recurrenceMonthDay: number | null;
  reminderOffsetMinutes: number;
  weakReminderMinutes: number | null;
  weakReminderEnabled: boolean;
  pausedFrom: string | null;
  pausedUntil: string | null;
  terminatedAt: string | null;
  createdAt: string;
  updatedAt: string;
}

export interface OccurrencePatch {
  title?: string;
  endAt?: string | null;
  location?: string | null;
  notes?: string | null;
}

export interface OccurrenceOverride {
  id: string;
  eventId: string;
  originalStartAt: string;
  newStartAt: string | null;
  patch: OccurrencePatch;
  status: OverrideStatus;
  createdAt: string;
  updatedAt: string;
}

export interface CalendarOccurrence {
  eventId: string;
  title: string;
  originalStartAt: string;
  effectiveStartAt: string;
  effectiveEndAt: string | null;
  kind: EventKind;
  timeZone: string;
  location: string | null;
  notes: string | null;
  recurrenceFrequency: RecurrenceFrequency | null;
  moved: boolean;
}

export interface WeakReminderInstance {
  id: string;
  eventId: string;
  originalStartAt: string;
  effectiveStartAt: string;
  triggerAt: string;
  status: WeakReminderStatus;
  deliveredAt: string | null;
  createdAt: string;
  updatedAt: string;
}

export interface ShortNote {
  id: string;
  content: string;
  category: string | null;
  expiresAt: string;
  createdAt: string;
}

export interface UndoOperation {
  id: string;
  action: string;
  summary: string;
  snapshot: Record<string, unknown>;
  expiresAt: string;
  undoneAt: string | null;
  createdAt: string;
}

export interface ReminderInstance {
  id: string;
  eventId: string;
  originalStartAt: string;
  effectiveStartAt: string;
  triggerAt: string;
  status: ReminderStatus;
  snoozeCount: number;
  voiceDeliveredAt: string | null;
  closedAt: string | null;
  createdAt: string;
  updatedAt: string;
}

export interface Receipt {
  id: string;
  type:
    | "calendar_created"
    | "calendar_query"
    | "calendar_rescheduled"
    | "calendar_modified"
    | "calendar_skipped"
    | "calendar_paused"
    | "calendar_resumed"
    | "calendar_terminated"
    | "calendar_deleted"
    | "calendar_undone"
    | "note_recorded"
    | "note_query"
    | "reminder_due"
    | "reminder_weak_due"
    | "reminder_closed"
    | "reminder_snoozed";
  eventId: string | null;
  reminderId: string | null;
  title: string;
  body: string;
  data: Record<string, unknown>;
  createdAt: string;
}

export interface RecurrenceInput {
  frequency: RecurrenceFrequency;
  weekday?: number;
  monthDay?: number;
}
