import crypto from "node:crypto";

function iso(now) {
  return new Date(now).toISOString();
}

function randomCode() {
  const alphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  const bytes = crypto.randomBytes(6);
  return Array.from(bytes, (byte) => alphabet[byte % alphabet.length]).join("");
}

function parseDueAt(value, now) {
  const timestamp = Date.parse(value);
  if (!Number.isFinite(timestamp)) {
    throw new Error("dueAt 必须是带时区的 ISO 8601 时间，例如 2026-07-30T15:00:00+08:00");
  }
  if (timestamp < now - 60_000) throw new Error("提醒时间不能早于当前时间一分钟以上");
  return new Date(timestamp).toISOString();
}

export class VoiceLifeService {
  constructor({ store, wechatApi, now = () => Date.now() }) {
    this.store = store;
    this.wechatApi = wechatApi;
    this.now = now;
  }

  createBindingCode(deviceId, ttlMs = 10 * 60 * 1000) {
    if (!deviceId) throw new Error("deviceId 不能为空");
    let code;
    do code = randomCode();
    while (this.store.state.bindCodes[code]);
    const createdAt = iso(this.now());
    const expiresAt = iso(this.now() + ttlMs);
    this.store.mutate((state) => {
      state.bindCodes[code] = { code, deviceId, createdAt, expiresAt, usedAt: null };
    });
    return { code, deviceId, expiresAt };
  }

  bindExternalIdentity(code, externalIdentity) {
    const normalized = String(code || "").trim().toUpperCase().replace(/^QRSCENE_/, "");
    const binding = this.store.state.bindCodes[normalized];
    if (!binding) throw new Error("绑定码不存在");
    if (binding.usedAt) throw new Error("绑定码已经使用");
    if (Date.parse(binding.expiresAt) < this.now()) throw new Error("绑定码已经过期");
    if (!externalIdentity?.platform || !externalIdentity?.userId) {
      throw new Error("ExternalIdentity 不能为空");
    }
    const boundAt = iso(this.now());
    this.store.mutate((state) => {
      state.bindCodes[normalized].usedAt = boundAt;
      state.bindings[binding.deviceId] = {
        deviceId: binding.deviceId,
        externalIdentity: {
          platform: String(externalIdentity.platform),
          userId: String(externalIdentity.userId)
        },
        boundAt,
        active: true
      };
    });
    return structuredClone(this.store.state.bindings[binding.deviceId]);
  }

  findDeviceByExternalIdentity(externalIdentity) {
    return Object.values(this.store.state.bindings).find(
      (binding) =>
        binding.externalIdentity.platform === externalIdentity.platform &&
        binding.externalIdentity.userId === externalIdentity.userId &&
        binding.active
    );
  }

  deactivateExternalIdentity(externalIdentity) {
    const binding = this.findDeviceByExternalIdentity(externalIdentity);
    if (!binding) return false;
    this.store.mutate((state) => {
      state.bindings[binding.deviceId].active = false;
    });
    return true;
  }

  createReminder({ deviceId, title, dueAt }) {
    if (!deviceId) throw new Error("deviceId 不能为空");
    if (!String(title || "").trim()) throw new Error("提醒内容不能为空");
    const reminder = {
      id: crypto.randomUUID(),
      deviceId,
      title: String(title).trim().slice(0, 200),
      dueAt: parseDueAt(dueAt, this.now()),
      status: "scheduled",
      createdAt: iso(this.now()),
      sentAt: null,
      dismissedAt: null,
      snoozeCount: 0,
      platformMessageId: null,
      deliveryStatus: null,
      error: null
    };
    this.store.mutate((state) => {
      state.reminders[reminder.id] = reminder;
    });
    return structuredClone(reminder);
  }

  listReminders(deviceId, { includeDone = false } = {}) {
    return Object.values(this.store.state.reminders)
      .filter((item) => item.deviceId === deviceId)
      .filter((item) => includeDone || ["scheduled", "sent"].includes(item.status))
      .sort((left, right) => Date.parse(left.dueAt) - Date.parse(right.dueAt))
      .map((item) => structuredClone(item));
  }

  dismissTarget(target) {
    const dismissedAt = iso(this.now());
    this.store.mutate((state) => {
      state.reminders[target.id].status = "dismissed";
      state.reminders[target.id].dismissedAt = dismissedAt;
    });
    return structuredClone(this.store.state.reminders[target.id]);
  }

  dismissForDevice({ deviceId, reminderId }) {
    const candidates = this.listReminders(deviceId);
    const target = reminderId
      ? this.store.state.reminders[reminderId]
      : candidates[0];
    if (!target || target.deviceId !== deviceId) throw new Error("提醒不存在");
    return this.dismissTarget(target);
  }

  findActionIntentTarget({ reminderId, dueAt }) {
    const target = this.store.state.reminders[reminderId];
    if (!target || target.dueAt !== dueAt) throw new Error("这个提醒链接已经失效");
    if (!["sent", "dismissed"].includes(target.status)) {
      throw new Error("这个提醒已经处理，请返回微信查看最新消息");
    }
    return target;
  }

  executeReminderAction({ reminderId, dueAt, action, minutes = 10 }) {
    const target = this.findActionIntentTarget({ reminderId, dueAt });
    if (action === "dismiss") {
      if (target.status === "dismissed") return structuredClone(target);
      return this.dismissTarget(target);
    }
    if (action === "snooze") {
      if (target.status !== "sent") throw new Error("这个提醒已经处理");
      const amount = Number(minutes);
      if (!Number.isInteger(amount) || amount < 1 || amount > 1440) {
        throw new Error("推迟分钟数必须是 1～1440 的整数");
      }
      return this.snoozeTarget(target, amount);
    }
    throw new Error("未知操作");
  }

  snoozeTarget(target, minutes) {
    const dueAt = iso(this.now() + minutes * 60_000);
    this.store.mutate((state) => {
      const reminder = state.reminders[target.id];
      reminder.status = "scheduled";
      reminder.dueAt = dueAt;
      reminder.sentAt = null;
      reminder.platformMessageId = null;
      reminder.deliveryStatus = null;
      reminder.snoozeCount += 1;
    });
    return structuredClone(this.store.state.reminders[target.id]);
  }

  snoozeForDevice({ deviceId, reminderId, minutes = 10 }) {
    const candidates = this.listReminders(deviceId);
    const target = reminderId
      ? this.store.state.reminders[reminderId]
      : candidates[0];
    if (!target || target.deviceId !== deviceId) throw new Error("提醒不存在");
    const amount = Number(minutes);
    if (!Number.isInteger(amount) || amount < 1 || amount > 1440) {
      throw new Error("推迟分钟数必须是 1～1440 的整数");
    }
    return this.snoozeTarget(target, amount);
  }

  async dispatchDue() {
    const due = Object.values(this.store.state.reminders)
      .filter((item) => item.status === "scheduled")
      .filter((item) => Date.parse(item.dueAt) <= this.now());
    const results = [];
    for (const reminder of due) {
      results.push(await this.dispatchReminder(reminder.id));
    }
    return results;
  }

  async dispatchReminder(reminderId) {
    const reminder = this.store.state.reminders[reminderId];
    if (!reminder) throw new Error("提醒不存在");
    if (reminder.status !== "scheduled") {
      return {
        ok: reminder.status === "sent",
        reminderId,
        skipped: true,
        status: reminder.status
      };
    }
    const binding = this.store.state.bindings[reminder.deviceId];
    if (!binding?.active) {
      this.store.mutate((state) => {
        state.reminders[reminderId].status = "failed";
        state.reminders[reminderId].error = "设备没有绑定有效的微信公众号用户";
      });
      return { ok: false, reminderId, error: "not_bound" };
    }
    try {
      if (binding.externalIdentity.platform !== "wechat-official") {
        throw new Error(`Demo 尚未安装 ${binding.externalIdentity.platform} 投递 Adapter`);
      }
      const response = await this.wechatApi.sendTemplate({
        openId: binding.externalIdentity.userId,
        reminder
      });
      const sentAt = iso(this.now());
      this.store.mutate((state) => {
        const current = state.reminders[reminderId];
        current.status = "sent";
        current.sentAt = sentAt;
        current.platformMessageId = String(response.msgid);
        current.deliveryStatus = "accepted";
        current.error = null;
        state.outbound.push({
          reminderId,
          externalIdentity: binding.externalIdentity,
          sentAt,
          response
        });
      });
      return { ok: true, reminderId, response };
    } catch (error) {
      this.store.mutate((state) => {
        state.reminders[reminderId].status = "failed";
        state.reminders[reminderId].error = error.message;
      });
      return { ok: false, reminderId, error: error.message };
    }
  }

  recordTemplateReceipt({ messageId, status, openId }) {
    const receivedAt = iso(this.now());
    const reminder = Object.values(this.store.state.reminders).find(
      (item) => String(item.platformMessageId) === String(messageId)
    );
    this.store.mutate((state) => {
      state.receipts.push({ messageId: String(messageId), status, openId, receivedAt });
      if (reminder) {
        state.reminders[reminder.id].deliveryStatus = status || "unknown";
      }
    });
    return { matched: Boolean(reminder), reminderId: reminder?.id ?? null };
  }
}
