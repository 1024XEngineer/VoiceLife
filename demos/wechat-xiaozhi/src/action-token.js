import crypto from "node:crypto";

function encode(value) {
  return Buffer.from(value).toString("base64url");
}

function signature(payload, secret) {
  return crypto.createHmac("sha256", secret).update(payload).digest("base64url");
}

export function createReminderActionToken({
  reminder,
  secret,
  now = Date.now(),
  ttlSeconds = 7 * 24 * 60 * 60
}) {
  if (!secret) throw new Error("提醒操作签名密钥未配置");
  const payload = encode(JSON.stringify({
    reminderId: reminder.id,
    dueAt: reminder.dueAt,
    exp: Math.floor(now / 1000) + ttlSeconds
  }));
  return `${payload}.${signature(payload, secret)}`;
}

export function verifyReminderActionToken(token, secret, now = Date.now()) {
  if (!secret) throw new Error("提醒操作签名密钥未配置");
  const [payload, receivedSignature, extra] = String(token || "").split(".");
  if (!payload || !receivedSignature || extra) throw new Error("操作链接无效");

  const expectedSignature = signature(payload, secret);
  const received = Buffer.from(receivedSignature);
  const expected = Buffer.from(expectedSignature);
  if (received.length !== expected.length || !crypto.timingSafeEqual(received, expected)) {
    throw new Error("操作链接签名无效");
  }

  let value;
  try {
    value = JSON.parse(Buffer.from(payload, "base64url").toString("utf8"));
  } catch {
    throw new Error("操作链接无效");
  }
  if (!value.reminderId || !value.dueAt || !Number.isFinite(value.exp)) {
    throw new Error("操作链接无效");
  }
  if (value.exp < Math.floor(now / 1000)) throw new Error("操作链接已过期");
  return value;
}
