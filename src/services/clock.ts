import { DateTime } from "luxon";

export interface Clock {
  now(): DateTime;
}

export class SystemClock implements Clock {
  public constructor(private readonly timeZone: string) {}

  now(): DateTime {
    return DateTime.now().setZone(this.timeZone);
  }
}

export class DemoClock implements Clock {
  private virtualAnchorMs: number;
  private realAnchorMs: number;

  public constructor(
    private readonly timeZone: string,
    initialNow = DateTime.now().setZone(timeZone),
  ) {
    this.virtualAnchorMs = initialNow.toMillis();
    this.realAnchorMs = Date.now();
  }

  now(): DateTime {
    const elapsed = Date.now() - this.realAnchorMs;
    return DateTime.fromMillis(this.virtualAnchorMs + elapsed, { zone: this.timeZone });
  }

  set(iso: string): DateTime {
    const next = DateTime.fromISO(iso, { zone: this.timeZone, setZone: true }).setZone(this.timeZone);
    if (!next.isValid) throw new Error("无效的演示时间");
    this.virtualAnchorMs = next.toMillis();
    this.realAnchorMs = Date.now();
    return this.now();
  }

  advance(minutes: number): DateTime {
    if (!Number.isFinite(minutes)) throw new Error("推进分钟数必须是数字");
    const next = this.now().plus({ minutes });
    this.virtualAnchorMs = next.toMillis();
    this.realAnchorMs = Date.now();
    return this.now();
  }

  reset(): DateTime {
    const next = DateTime.now().setZone(this.timeZone);
    this.virtualAnchorMs = next.toMillis();
    this.realAnchorMs = Date.now();
    return this.now();
  }
}
