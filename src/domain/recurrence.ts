import { DateTime } from "luxon";
import type { CalendarEvent } from "./types.js";

export function parseDateTime(iso: string, timeZone: string): DateTime {
  const parsed = DateTime.fromISO(iso, { setZone: true, zone: timeZone }).setZone(timeZone);
  if (!parsed.isValid) throw new Error(`无效时间：${iso}`);
  return parsed;
}

export function toStorageIso(value: DateTime): string {
  const iso = value.toUTC().toISO({ suppressMilliseconds: false });
  if (!iso) throw new Error("无法序列化时间");
  return iso;
}

export function nextOccurrence(event: CalendarEvent, originalStartAt: string): DateTime | null {
  if (!event.recurrenceFrequency) return null;
  const current = parseDateTime(originalStartAt, event.timeZone);
  const termination = event.terminatedAt
    ? parseDateTime(event.terminatedAt, event.timeZone)
    : null;

  const withinSeries = (candidate: DateTime): DateTime | null =>
    termination && candidate >= termination ? null : candidate;

  if (event.recurrenceFrequency === "daily") return withinSeries(current.plus({ days: 1 }));
  if (event.recurrenceFrequency === "weekly") return withinSeries(current.plus({ weeks: 1 }));

  const requestedDay = event.recurrenceMonthDay ?? current.day;
  let candidateMonth = current.plus({ months: 1 }).startOf("month");
  for (let attempt = 0; attempt < 24; attempt += 1) {
    if (requestedDay <= candidateMonth.daysInMonth!) {
      return withinSeries(candidateMonth.set({
        day: requestedDay,
        hour: current.hour,
        minute: current.minute,
        second: current.second,
        millisecond: current.millisecond,
      }));
    }
    candidateMonth = candidateMonth.plus({ months: 1 });
  }
  throw new Error("无法计算下一次月度日程");
}

export function originalOccurrencesBetween(
  event: CalendarEvent,
  rangeStart: DateTime,
  rangeEnd: DateTime,
  limit = 500,
): string[] {
  const results: string[] = [];
  let current = parseDateTime(event.startAt, event.timeZone);
  const termination = event.terminatedAt
    ? parseDateTime(event.terminatedAt, event.timeZone)
    : null;

  for (let index = 0; index < limit && current < rangeEnd; index += 1) {
    if (termination && current >= termination) break;
    if (current >= rangeStart) results.push(toStorageIso(current));
    const next = nextOccurrence(event, toStorageIso(current));
    if (!next) break;
    current = next;
  }

  return results;
}

export function formatChineseDateTime(iso: string, timeZone: string): string {
  return parseDateTime(iso, timeZone).toFormat("yyyy年M月d日 HH:mm");
}
