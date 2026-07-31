import assert from "node:assert/strict";
import test from "node:test";
import { VoiceLifeService } from "../src/service.js";
import { MemoryStore } from "../src/store.js";

function fixture() {
  let now = Date.parse("2026-07-29T10:00:00+08:00");
  const sent = [];
  const store = new MemoryStore();
  const wechatApi = {
    async sendTemplate(input) {
      sent.push(input);
      return { errcode: 0, errmsg: "ok", msgid: "wx-message-1" };
    }
  };
  const service = new VoiceLifeService({
    store,
    wechatApi,
    now: () => now
  });
  return {
    service,
    store,
    sent,
    advance(ms) {
      now += ms;
    }
  };
}

test("绑定码把小智设备和平台 ExternalIdentity 关联", () => {
  const { service } = fixture();
  const token = service.createBindingCode("xiaozhi-01");
  const identity = { platform: "wechat-official", userId: "openid-01" };
  const binding = service.bindExternalIdentity(token.code, identity);
  assert.equal(binding.deviceId, "xiaozhi-01");
  assert.deepEqual(binding.externalIdentity, identity);
  assert.throws(
    () => service.bindExternalIdentity(token.code, {
      platform: "wechat-official",
      userId: "other"
    }),
    /已经使用/
  );
});

test("到期提醒发送模板并记录发送回执", async () => {
  const { service, store, sent, advance } = fixture();
  const token = service.createBindingCode("xiaozhi-01");
  service.bindExternalIdentity(token.code, {
    platform: "wechat-official",
    userId: "openid-01"
  });
  const reminder = service.createReminder({
    deviceId: "xiaozhi-01",
    title: "喝水",
    dueAt: "2026-07-29T10:01:00+08:00"
  });
  advance(61_000);
  const results = await service.dispatchDue();
  assert.equal(results[0].ok, true);
  assert.equal(sent[0].openId, "openid-01");
  assert.equal(store.state.reminders[reminder.id].status, "sent");
  assert.equal(store.state.reminders[reminder.id].deliveryStatus, "accepted");

  const receipt = service.recordTemplateReceipt({
    messageId: "wx-message-1",
    status: "success",
    openId: "openid-01"
  });
  assert.equal(receipt.reminderId, reminder.id);
  assert.equal(store.state.reminders[reminder.id].deliveryStatus, "success");
});

test("小智可以直接关闭和推迟提醒", () => {
  const { service, advance } = fixture();
  const first = service.createReminder({
    deviceId: "xiaozhi-01",
    title: "第一条",
    dueAt: "2026-07-29T10:01:00+08:00"
  });
  assert.equal(
    service.snoozeForDevice({ deviceId: "xiaozhi-01", minutes: 15 }).snoozeCount,
    1
  );
  advance(1_000);
  const dismissed = service.dismissForDevice({
    deviceId: "xiaozhi-01",
    reminderId: first.id
  });
  assert.equal(dismissed.status, "dismissed");
});

test("统一动作入口可以知道了或推迟，旧动作令牌不能重复推迟", async () => {
  const { service, advance } = fixture();
  const binding = service.createBindingCode("xiaozhi-01");
  service.bindExternalIdentity(binding.code, {
    platform: "wechat-official",
    userId: "openid-01"
  });
  const dismissed = service.createReminder({
    deviceId: "xiaozhi-01",
    title: "吃药",
    dueAt: "2026-07-29T10:01:00+08:00"
  });
  advance(61_000);
  await service.dispatchReminder(dismissed.id);
  assert.equal(
    service.executeReminderAction({
      reminderId: dismissed.id,
      dueAt: dismissed.dueAt,
      action: "dismiss"
    }).status,
    "dismissed"
  );

  const snoozed = service.createReminder({
    deviceId: "xiaozhi-01",
    title: "喝水",
    dueAt: "2026-07-29T10:02:30+08:00"
  });
  advance(30_000);
  await service.dispatchReminder(snoozed.id);
  const action = { reminderId: snoozed.id, dueAt: snoozed.dueAt };
  const updated = service.executeReminderAction({
    ...action,
    action: "snooze",
    minutes: 10
  });
  assert.equal(updated.status, "scheduled");
  assert.equal(updated.snoozeCount, 1);
  assert.throws(
    () => service.executeReminderAction({
      ...action,
      action: "snooze",
      minutes: 10
    }),
    /已经失效/
  );
});
