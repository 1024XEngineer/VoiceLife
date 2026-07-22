import { describe, expect, it } from "vitest";
import { buildProactiveReminderText } from "../src/adapters/linx-mac-proactive-voice.js";
import type { ProactiveVoiceItem } from "../src/adapters/proactive-voice.js";

function item(title: string, snoozeCount: number): ProactiveVoiceItem {
  return {
    reminder: { snoozeCount } as ProactiveVoiceItem["reminder"],
    occurrence: {
      title,
      effectiveStartAt: "2026-07-21T01:00:00.000Z",
      timeZone: "Asia/Shanghai",
    } as ProactiveVoiceItem["occurrence"],
  };
}

describe("proactive reminder speech", () => {
  it("combines simultaneous first-time reminders", () => {
    expect(buildProactiveReminderText([
      item("开会", 0),
      item("吃饭", 0),
    ])).toBe("提醒：你有2件事情到时间了：09点的开会、09点的吃饭。");
  });

  it("identifies simultaneous reminders that are sounding again", () => {
    expect(buildProactiveReminderText([
      item("开会", 1),
      item("吃饭", 1),
    ])).toBe("再次提醒：你之前推迟的2件事情到时间了：09点的开会、09点的吃饭。");
  });

  it("distinguishes a weak reminder that needs no response", () => {
    expect(buildProactiveReminderText([{ ...item("路演", 0), deliveryKind: "weak" }]))
      .toBe("提前提示：路演将在十五分钟后，也就是09点开始。");
  });
});
