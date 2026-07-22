import type { CalendarOccurrence, ReminderInstance } from "../domain/types.js";

export interface ProactiveVoiceDeliveryResult {
  status: "delivered" | "unsupported" | "failed";
  detail?: string;
}

export interface ProactiveVoiceItem {
  reminder: ReminderInstance;
  occurrence: CalendarOccurrence;
  deliveryKind?: "main" | "weak";
}

export interface ProactiveVoiceAdapter {
  deliver(
    reminder: ReminderInstance,
    occurrence: CalendarOccurrence,
    deliveryKind?: "main" | "weak",
  ): Promise<ProactiveVoiceDeliveryResult>;
  deliverBatch?(items: ProactiveVoiceItem[]): Promise<ProactiveVoiceDeliveryResult>;
  close?(): Promise<void>;
}

export class UnsupportedProactiveVoiceAdapter implements ProactiveVoiceAdapter {
  async deliver(): Promise<ProactiveVoiceDeliveryResult> {
    return {
      status: "unsupported",
      detail: "未配置灵矽 macOS 主动语音客户端",
    };
  }
}
