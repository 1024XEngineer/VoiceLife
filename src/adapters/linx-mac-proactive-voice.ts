import type { CalendarOccurrence, ReminderInstance } from "../domain/types.js";
import { LinxMacVoiceClient } from "../clients/linx-mac-voice-client.js";
import type {
  ProactiveVoiceAdapter,
  ProactiveVoiceDeliveryResult,
  ProactiveVoiceItem,
} from "./proactive-voice.js";
import { DateTime } from "luxon";

function spokenTime(iso: string, timeZone: string): string {
  return DateTime.fromISO(iso, { setZone: true }).setZone(timeZone).toFormat("HH点mm分").replace("点00分", "点");
}

export function buildProactiveReminderText(items: ProactiveVoiceItem[]): string {
  if (items.length === 0) throw new Error("没有可播报的到期提醒");
  const allSnoozed = items.every((item) => item.reminder.snoozeCount > 0);
  if (items.length === 1) {
    const item = items[0]!;
    if (item.deliveryKind === "weak") {
      return `提前提示：${item.occurrence.title}将在十五分钟后，也就是${spokenTime(item.occurrence.effectiveStartAt, item.occurrence.timeZone)}开始。`;
    }
    const prefix = allSnoozed ? "再次提醒" : "提醒";
    return `${prefix}：${spokenTime(item.occurrence.effectiveStartAt, item.occurrence.timeZone)}的${item.occurrence.title}到时间了。`;
  }
  const spoken = items.slice(0, 3).map((item) =>
    item.deliveryKind === "weak"
      ? `${item.occurrence.title}将在十五分钟后开始`
      : `${spokenTime(item.occurrence.effectiveStartAt, item.occurrence.timeZone)}的${item.occurrence.title}`,
  ).join("、");
  const remaining = items.length - 3;
  const suffix = remaining > 0 ? `，另外还有${remaining}件，请查看消息` : "";
  const lead = allSnoozed
    ? `再次提醒：你之前推迟的${items.length}件事情到时间了`
    : `提醒：你有${items.length}件事情到时间了`;
  return `${lead}：${spoken}${suffix}。`;
}

export class LinxMacProactiveVoiceAdapter implements ProactiveVoiceAdapter {
  public constructor(private readonly client: LinxMacVoiceClient) {}

  async deliver(
    reminder: ReminderInstance,
    occurrence: CalendarOccurrence,
    deliveryKind: "main" | "weak" = "main",
  ): Promise<ProactiveVoiceDeliveryResult> {
    return this.deliverBatch([{ reminder, occurrence, deliveryKind }]);
  }

  async deliverBatch(items: ProactiveVoiceItem[]): Promise<ProactiveVoiceDeliveryResult> {
    if (items.length === 0) {
      return { status: "failed", detail: "没有可播报的到期提醒" };
    }
    const reminderText = buildProactiveReminderText(items);
    const prompt = `【系统到期播报】请只重复下一行内容，不要调用工具，不要添加解释。\n${reminderText}`;
    try {
      const playback = await this.client.speak(prompt);
      return {
        status: "delivered",
        detail: `灵矽 ${playback.format} 音频已通过 macOS 扬声器播放`,
      };
    } catch (error) {
      return {
        status: "failed",
        detail: error instanceof Error ? error.message : "未知的灵矽语音播放错误",
      };
    }
  }

  close(): Promise<void> {
    return this.client.close();
  }
}
