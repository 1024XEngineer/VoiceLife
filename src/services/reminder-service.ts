import { randomUUID } from "node:crypto";
import type {
  CalendarOccurrence,
  Receipt,
  ReminderInstance,
} from "../domain/types.js";
import { formatChineseDateTime, toStorageIso } from "../domain/recurrence.js";
import type { ProactiveVoiceAdapter } from "../adapters/proactive-voice.js";
import { CalendarDatabase } from "../storage/database.js";
import type { Clock } from "./clock.js";
import { CalendarService } from "./calendar-service.js";
import { ReceiptBus } from "./receipt-bus.js";

export interface DueReminder {
  reminder: ReminderInstance;
  occurrence: CalendarOccurrence;
  deliveryKind?: "main" | "weak";
}

export class ReminderService {
  public constructor(
    private readonly db: CalendarDatabase,
    private readonly calendarService: CalendarService,
    private readonly clock: Clock,
    private readonly receiptBus: ReceiptBus,
    private readonly proactiveVoice: ProactiveVoiceAdapter,
  ) {}

  async scanDue(): Promise<DueReminder[]> {
    const now = toStorageIso(this.clock.now());
    const ready = this.db.listReadyToPush(now);
    const pushed: DueReminder[] = [];
    const weakDue = this.scanWeakDue(now);

    for (const reminder of ready) {
      const occurrence = this.calendarService.getOccurrenceForReminder(reminder);
      const receipt = this.buildReceipt({
        type: "reminder_due",
        eventId: reminder.eventId,
        reminderId: reminder.id,
        title: reminder.snoozeCount > 0 ? "之前推迟的提醒" : "提醒到达",
        body: `${occurrence.title} · ${formatChineseDateTime(occurrence.effectiveStartAt, occurrence.timeZone)}`,
        data: {
          reminderId: reminder.id,
          title: occurrence.title,
          effectiveStartAt: occurrence.effectiveStartAt,
          snoozeCount: reminder.snoozeCount,
          finalDelivery: reminder.snoozeCount >= 3,
        },
        createdAt: now,
      });

      this.db.transaction(() => {
        this.db.markReminderPushed(reminder.id, now);
        this.db.insertReceipt(receipt);
        this.calendarService.ensureNextReminder(reminder.eventId, reminder.originalStartAt);
      });
      this.receiptBus.publish(receipt);
      const updated = this.db.getReminder(reminder.id)!;
      pushed.push({ reminder: updated, occurrence });
    }

    const voiceEligible = pushed.filter((item) => item.reminder.snoozeCount < 3);
    await this.deliverProactiveVoice(
      [...weakDue, ...voiceEligible].sort((left, right) =>
        left.reminder.triggerAt.localeCompare(right.reminder.triggerAt),
      ),
    );
    return pushed.map((item) => ({
      ...item,
      reminder: this.db.getReminder(item.reminder.id)!,
    }));
  }

  listDue(): DueReminder[] {
    const now = toStorageIso(this.clock.now());
    const due = this.db.listPushed().map((reminder) => ({
      reminder,
      occurrence: this.calendarService.getOccurrenceForReminder(reminder),
    }));
    for (const item of due) this.db.markReminderVoiceDelivered(item.reminder.id, now);
    return due.map((item) => ({
      ...item,
      reminder: this.db.getReminder(item.reminder.id)!,
    }));
  }

  close(reminderId: string): { reminder: ReminderInstance; receipt: Receipt | null; alreadyClosed: boolean } {
    const reminder = this.requireReminder(reminderId);
    if (reminder.status === "closed") {
      return { reminder, receipt: null, alreadyClosed: true };
    }
    const occurrence = this.calendarService.getOccurrenceForReminder(reminder);
    const now = toStorageIso(this.clock.now());
    const receipt = this.buildReceipt({
      type: "reminder_closed",
      eventId: reminder.eventId,
      reminderId: reminder.id,
      title: "已关闭提醒",
      body: `${occurrence.title} · 日程保持不变`,
      data: { reminderId, title: occurrence.title, scheduleChanged: false },
      createdAt: now,
    });
    this.db.transaction(() => {
      this.db.closeReminder(reminderId, now);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return { reminder: this.requireReminder(reminderId), receipt, alreadyClosed: false };
  }

  snooze(reminderId: string, minutes: number): {
    reminder: ReminderInstance;
    receipt: Receipt | null;
    alreadySnoozed: boolean;
  } {
    if (!Number.isInteger(minutes) || minutes < 1 || minutes > 24 * 60) {
      throw new Error("推迟时间必须是 1 到 1440 分钟的整数");
    }
    const reminder = this.requireReminder(reminderId);
    if (reminder.status === "snoozed") {
      return { reminder, receipt: null, alreadySnoozed: true };
    }
    if (reminder.status === "closed") throw new Error("这条提醒已经关闭");
    if (reminder.status !== "pushed") throw new Error("这条提醒当前不能推迟");
    if (reminder.snoozeCount >= 3) throw new Error("这条提醒已经推迟三次，不能继续推迟");

    const occurrence = this.calendarService.getOccurrenceForReminder(reminder);
    const nowDateTime = this.clock.now();
    const now = toStorageIso(nowDateTime);
    const nextTriggerAt = toStorageIso(nowDateTime.plus({ minutes }));
    const nextCount = reminder.snoozeCount + 1;
    const receipt = this.buildReceipt({
      type: "reminder_snoozed",
      eventId: reminder.eventId,
      reminderId: reminder.id,
      title: "已推迟提醒",
      body: `${occurrence.title} · ${minutes} 分钟后再次提醒`,
      data: {
        reminderId,
        title: occurrence.title,
        minutes,
        nextTriggerAt,
        snoozeCount: nextCount,
        scheduleChanged: false,
      },
      createdAt: now,
    });
    this.db.transaction(() => {
      this.db.snoozeReminder(reminderId, nextTriggerAt, nextCount, now);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return {
      reminder: this.requireReminder(reminderId),
      receipt,
      alreadySnoozed: false,
    };
  }

  getDetails(reminderId: string): DueReminder {
    const reminder = this.requireReminder(reminderId);
    return {
      reminder,
      occurrence: this.calendarService.getOccurrenceForReminder(reminder),
    };
  }

  private requireReminder(id: string): ReminderInstance {
    const reminder = this.db.getReminder(id);
    if (!reminder) throw new Error("没有找到对应提醒");
    return reminder;
  }

  private scanWeakDue(now: string): DueReminder[] {
    const due: DueReminder[] = [];
    for (const weak of this.db.listReadyWeakReminders(now)) {
      if (weak.effectiveStartAt <= now) {
        this.db.markWeakReminderDelivered(weak.id, now);
        continue;
      }
      const occurrence = this.calendarService.getOccurrence(weak.eventId, weak.originalStartAt);
      const receipt = this.buildReceipt({
        type: "reminder_weak_due",
        eventId: weak.eventId,
        reminderId: null,
        title: "提前提示",
        body: `${occurrence.title} · 15 分钟后开始 · ${formatChineseDateTime(occurrence.effectiveStartAt, occurrence.timeZone)}`,
        data: {
          weakReminderId: weak.id,
          title: occurrence.title,
          effectiveStartAt: occurrence.effectiveStartAt,
          requiresResponse: false,
        },
        createdAt: now,
      });
      this.db.transaction(() => {
        this.db.markWeakReminderDelivered(weak.id, now);
        this.db.insertReceipt(receipt);
      });
      this.receiptBus.publish(receipt);
      due.push({
        occurrence,
        deliveryKind: "weak",
        reminder: {
          id: weak.id,
          eventId: weak.eventId,
          originalStartAt: weak.originalStartAt,
          effectiveStartAt: weak.effectiveStartAt,
          triggerAt: weak.triggerAt,
          status: "pushed",
          snoozeCount: 0,
          voiceDeliveredAt: null,
          closedAt: null,
          createdAt: weak.createdAt,
          updatedAt: now,
        },
      });
    }
    return due;
  }

  private async deliverProactiveVoice(items: DueReminder[]): Promise<void> {
    if (items.length === 0) return;
    if (items.length > 1 && this.proactiveVoice.deliverBatch) {
      const delivery = await this.proactiveVoice.deliverBatch(items);
      if (delivery.status === "delivered") {
        const deliveredAt = toStorageIso(this.clock.now());
        for (const item of items) {
          if (this.db.getReminder(item.reminder.id)) {
            this.db.markReminderVoiceDelivered(item.reminder.id, deliveredAt);
          }
        }
      } else if (delivery.status === "failed") {
        console.error(`Proactive voice delivery failed: ${delivery.detail ?? "unknown error"}`);
      }
      return;
    }

    for (const item of items) {
      const delivery = await this.proactiveVoice.deliver(
        item.reminder,
        item.occurrence,
        item.deliveryKind,
      );
      if (delivery.status === "delivered") {
        if (this.db.getReminder(item.reminder.id)) {
          this.db.markReminderVoiceDelivered(item.reminder.id, toStorageIso(this.clock.now()));
        }
      } else if (delivery.status === "failed") {
        console.error(`Proactive voice delivery failed: ${delivery.detail ?? "unknown error"}`);
      }
    }
  }

  private buildReceipt(input: Omit<Receipt, "id">): Receipt {
    return { id: randomUUID(), ...input };
  }
}
